/**
 * UHF 双层扫描器 — 实现
 *
 * 一次 init → start → 持续 poll → stop → deinit
 * EPC 白名单从 DB books 表动态加载
 */
#include "uhf_scanner.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── 内部 ────────────────────────────────────────────────── */

static int book_index(scanner_t *sc, const char *epc) {
    for (int i = 0; i < sc->book_count; i++)
        if (strcasecmp(sc->book_epc[i], epc) == 0) return i;
    return -1;
}

static void emit_event(scanner_t *sc, scan_event_type_t t,
                       const char *epc, const char *title, int layer, int rssi) {
    if (sc->event_count >= SCANNER_MAX_EVENTS) return;
    scan_event_t *ev = &sc->events[sc->event_count++];
    memset(ev, 0, sizeof(*ev));
    ev->type = t;
    snprintf(ev->epc, sizeof(ev->epc), "%.64s", epc ? epc : "");
    if (title) snprintf(ev->title, sizeof(ev->title), "%.127s", title);
    ev->layer = layer;
    ev->expected_layer = layer;
    ev->rssi = rssi;
}

static int unknown_recent(scanner_t *sc, const char *epc, uint32_t now)
{
    for (int i = 0; i < sc->unknown_count; ++i) {
        if (strcasecmp(sc->unknown_epc[i], epc) == 0) {
            if (now - sc->unknown_seen[i] < 20) {
                sc->unknown_seen[i] = now;
                return 1;
            }
            sc->unknown_seen[i] = now;
            return 0;
        }
    }
    if (sc->unknown_count < SCANNER_MAX_EVENTS) {
        snprintf(sc->unknown_epc[sc->unknown_count],
                 sizeof(sc->unknown_epc[sc->unknown_count]), "%.64s", epc);
        sc->unknown_seen[sc->unknown_count] = now;
        sc->unknown_count++;
    }
    return 0;
}

static int choose_layer_from_reads(scanner_t *sc, int book, int fallback)
{
    int up = sc->layer_score[book][0];
    int lo = sc->layer_score[book][1];
    int total = up + lo;
    int main_layer = (lo > up) ? 1 : 0;
    int main_score = (main_layer == 0) ? up : lo;
    int other_score = (main_layer == 0) ? lo : up;

    if (total < 6) return fallback;
    if (main_score >= other_score + 6) return main_layer;
    if (main_score >= 6 && main_score * 100 >= total * 70) return main_layer;
    return fallback;
}

static int choose_layer_from_rssi(int up_seen, int lo_seen, int up_rssi, int lo_rssi, int fallback)
{
    if (up_seen && !lo_seen) return 0;
    if (lo_seen && !up_seen) return 1;
    if (!up_seen && !lo_seen) return fallback;
    if (up_rssi >= lo_rssi + 10) return 0;
    if (lo_rssi >= up_rssi + 10) return 1;
    return fallback;
}

static int choose_layer_from_evidence(int delta_up, int delta_lo,
                                      int score_layer, int rssi_layer,
                                      int fallback)
{
    int delta_layer = fallback;
    int total = delta_up + delta_lo;
    int main_layer = (delta_lo > delta_up) ? 1 : 0;
    int main_delta = (main_layer == 0) ? delta_up : delta_lo;
    int other_delta = (main_layer == 0) ? delta_lo : delta_up;

    if (total >= 4 && (main_delta >= other_delta + 3 ||
                       main_delta * 100 >= total * 70)) {
        delta_layer = main_layer;
    }

    if (delta_layer != fallback) return delta_layer;
    if (score_layer != fallback) return score_layer;
    if (rssi_layer != fallback) return rssi_layer;
    return fallback;
}

static const char *layer_name(int layer)
{
    return layer == 0 ? "上" : layer == 1 ? "下" : "?";
}

/* ═══════════════════════════════════════════════════════════ */

int scanner_init(scanner_t *sc, db_ctx_t *db,
                 const char *up_dev, const char *lo_dev,
                 int up_power, int lo_power) {
    if (!sc || !db) return -1;
    memset(sc, 0, sizeof(*sc));
    sc->db = db;
    sc->power[0] = up_power;
    sc->power[1] = lo_power;
    sc->present_timeout_sec = 5;   /* 5 秒未读到 = 可能取走 */
    sc->return_confirm = 6;        /* 连续确认放回, 位置仍由 FSM 保守处理 */
    sc->missing_confirm = 8;       /* 连续缺读确认取走; FSM 再结合最近红外证据 */
    sc->layer_change_confirm = 6;  /* 主读层连续稳定变化后再报错放/归位 */

    /* 1. 从 DB 加载 EPC 列表 */
    book_info_t books[SCANNER_MAX_BOOKS];
    sc->book_count = db_book_list(db, books, SCANNER_MAX_BOOKS);
    if (sc->book_count < 0) sc->book_count = 0;

    for (int i = 0; i < sc->book_count; i++) {
        shelf_item_t shelf;
        snprintf(sc->book_epc[i], sizeof(sc->book_epc[i]), "%.64s", books[i].epc);
        snprintf(sc->book_title[i], sizeof(sc->book_title[i]), "%.127s", books[i].title);
        sc->book_layer[i] = books[i].expected_layer;
        sc->actual_layer[i] = books[i].expected_layer;
        sc->is_present[i] = 0;
        if (db_shelf_find(db, books[i].epc, &shelf) == 1) {
            sc->actual_layer[i] = shelf.layer;
            sc->is_present[i] = shelf.is_present;
        }
    }
    printf("[scanner] loaded %d books from DB\n", sc->book_count);

    if (sc->book_count == 0) {
        fprintf(stderr, "[scanner] WARNING: no books in DB, EPC matching disabled\n");
        /* 仍然继续 — 标签会被收集但不做匹配 */
    }

    /* 2. 初始化两个模块 */
    const char *devs[2] = { up_dev, lo_dev };
    const char *names[2] = { "upper", "lower" };

    for (int i = 0; i < 2; i++) {
        if (uhf_init(&sc->dev[i], devs[i]) < 0) {
            fprintf(stderr, "[scanner] FAIL: %s (%s) init\n", names[i], devs[i]);
            /* 失败不致命 — 一个模块坏了另一个还可以用 */
            continue;
        }
        printf("[scanner] %s: FW v%d.%d.%d\n", names[i],
               sc->dev[i].fw_ver[0], sc->dev[i].fw_ver[1], sc->dev[i].fw_ver[2]);

        uhf_set_power(&sc->dev[i], (uint8_t)sc->power[i]);
        uhf_tags_clear(&sc->dev[i]);

        if (uhf_inventory_start(&sc->dev[i]) < 0) {
            fprintf(stderr, "[scanner] FAIL: %s inventory_start\n", names[i]);
            continue;
        }
        printf("[scanner] %s: %ddBm, inventory started\n", names[i], sc->power[i]);
    }

    sc->ready = 1;
    sc->started_at = (uint32_t)time(NULL);
    return 0;
}

int scanner_poll(scanner_t *sc) {
    if (!sc || !sc->ready) return -1;

    sc->event_count = 0;
    uint32_t now = (uint32_t)time(NULL);

    /* poll 两个模块 */
    for (int i = 0; i < 2; i++) {
        if (sc->dev[i].fd < 0) continue;
        uhf_poll(&sc->dev[i], 10);  /* 10ms 超时, 多读帧提稳定性 */
    }

    /*
     * 检查每本已知书 — 搜索两个模块
     * 用 tag.last_seen (实际读到时间) + streak 确认防抖
     */
    for (int b = 0; b < sc->book_count; b++) {
        int found = 0; uint8_t rssi = 0; int actual_layer = sc->book_layer[b];
        int delta_side[2] = {0, 0};
        int seen_side[2] = {0, 0};
        int rssi_side[2] = {0, 0};

        for (int d = 0; d < 2; d++) {
            if (sc->dev[d].fd < 0) continue;
            int idx = uhf_tag_index(&sc->dev[d], sc->book_epc[b]);
            int delta = 0;

            sc->layer_score[b][d] = (sc->layer_score[b][d] * 70) / 100;
            if (idx < 0) continue;

            uhf_tag_t *tag = &sc->dev[d].tags[idx];
            uint32_t tag_seen = (uint32_t)tag->last_seen;
            if (tag_seen + 1 >= now) {  /* 1 秒内被读到 → 本轮有效, 减少手持残留 */
                found = 1;
                seen_side[d] = 1;
                delta = tag->read_count - sc->last_read_count[b][d];
                if (delta < 0) delta = tag->read_count;
                if (delta > 40) delta = 40;
                sc->last_read_count[b][d] = tag->read_count;
                sc->layer_score[b][d] += delta;
                delta_side[d] = delta;
                rssi_side[d] = tag->rssi;
                sc->last_rssi_side[b][d] = tag->rssi;
                if (tag->rssi > rssi) rssi = tag->rssi;
            }
        }

        if (found) {
            int prev_layer = sc->actual_layer[b];
            int score_layer = choose_layer_from_reads(sc, b, prev_layer);
            int rssi_layer = choose_layer_from_rssi(seen_side[0], seen_side[1],
                                                    rssi_side[0], rssi_side[1],
                                                    prev_layer);
            int observed_layer = choose_layer_from_evidence(delta_side[0], delta_side[1],
                                                            score_layer, rssi_layer,
                                                            prev_layer);
            int confirmed_layer_change = 0;
            actual_layer = observed_layer;

            if (actual_layer == sc->layer_candidate[b]) {
                sc->layer_candidate_streak[b]++;
            } else {
                sc->layer_candidate[b] = actual_layer;
                sc->layer_candidate_streak[b] = 1;
            }

            if (sc->layer_candidate_streak[b] >= sc->layer_change_confirm) {
                actual_layer = sc->layer_candidate[b];
                confirmed_layer_change = (actual_layer != prev_layer);
            } else {
                actual_layer = prev_layer;
            }

            sc->last_seen[b] = now;
            sc->actual_layer[b] = actual_layer;
            sc->last_rssi[b] = rssi;
            sc->miss_streak[b] = 0;
            if (!sc->is_present[b]) {
                if (sc->missing_reported_at[b] && now - sc->missing_reported_at[b] < 2) {
                    sc->seen_streak[b] = 0;
                } else {
                    sc->seen_streak[b]++;
                }
                if (sc->seen_streak[b] >= sc->return_confirm) {
                    sc->is_present[b] = 1;
                    sc->seen_streak[b] = 0;
                    sc->layer_change_streak[b] = 0;
                    sc->missing_reported_at[b] = 0;
                    if (actual_layer != sc->book_layer[b]) {
                        printf("[scanner] misplaced return: %s expected=%s actual=%s score=%d/%d delta=%d/%d rssi=%d/%d\n",
                               sc->book_title[b], layer_name(sc->book_layer[b]),
                               layer_name(actual_layer), sc->layer_score[b][0],
                               sc->layer_score[b][1], delta_side[0], delta_side[1],
                               rssi_side[0], rssi_side[1]);
                    }
                    emit_event(sc, SCAN_EV_RETURNED,
                               sc->book_epc[b], sc->book_title[b],
                               actual_layer, rssi);
                    sc->events[sc->event_count - 1].expected_layer = sc->book_layer[b];
                }
            } else if (confirmed_layer_change) {
                if (actual_layer != sc->book_layer[b]) {
                    printf("[scanner] misplaced while present: %s expected=%s actual=%s score=%d/%d delta=%d/%d rssi=%d/%d\n",
                           sc->book_title[b], layer_name(sc->book_layer[b]),
                           layer_name(actual_layer), sc->layer_score[b][0],
                           sc->layer_score[b][1], delta_side[0], delta_side[1],
                           rssi_side[0], rssi_side[1]);
                } else {
                    printf("[scanner] layer restored: %s actual=%s score=%d/%d\n",
                           sc->book_title[b], layer_name(actual_layer),
                           sc->layer_score[b][0], sc->layer_score[b][1]);
                }
                emit_event(sc, SCAN_EV_RETURNED,
                           sc->book_epc[b], sc->book_title[b],
                           actual_layer, rssi);
                sc->events[sc->event_count - 1].expected_layer = sc->book_layer[b];
            } else {
                sc->layer_change_streak[b] = 0;
            }
        } else {
            sc->seen_streak[b] = 0;
            sc->layer_change_streak[b] = 0;
            if (sc->is_present[b]) {
                sc->miss_streak[b]++;
                if (sc->miss_streak[b] >= sc->missing_confirm) {
                    sc->is_present[b] = 0;
                    sc->miss_streak[b] = 0;
                    sc->missing_reported_at[b] = now;
                    sc->layer_score[b][0] = 0;
                    sc->layer_score[b][1] = 0;
                    sc->layer_candidate[b] = sc->book_layer[b];
                    sc->layer_candidate_streak[b] = 0;
                    emit_event(sc, SCAN_EV_MISSING,
                               sc->book_epc[b], sc->book_title[b],
                               sc->book_layer[b], 0);
                    sc->events[sc->event_count - 1].expected_layer = sc->book_layer[b];
                }
            }
        }
    }

    if (sc->detect_unknown) {
        for (int d = 0; d < 2; d++) {
            if (sc->dev[d].fd < 0) continue;
            for (int t = 0; t < sc->dev[d].tag_count; ++t) {
                uhf_tag_t *tag = &sc->dev[d].tags[t];
                if ((uint32_t)tag->last_seen + 3 < now) continue;
                if (book_index(sc, tag->epc_str) >= 0) continue;
                if (unknown_recent(sc, tag->epc_str, now)) continue;
                emit_event(sc, SCAN_EV_UNKNOWN, tag->epc_str, "未登记书籍", d, tag->rssi);
                sc->detect_unknown = 0;
                sc->round_count++;
                return sc->event_count;
            }
        }
    }

    sc->round_count++;
    return sc->event_count;
}

void scanner_stop(scanner_t *sc) {
    if (!sc || !sc->ready) return;

    for (int i = 0; i < 2; i++) {
        if (sc->dev[i].fd >= 0) {
            uhf_inventory_stop(&sc->dev[i]);
            uhf_deinit(&sc->dev[i]);
        }
    }

    /* 停机只确保 shelf_state 存在。位置/在架状态由 FSM 操作窗口持久化。 */
    for (int b = 0; b < sc->book_count; b++) {
        shelf_item_t prev;
        book_info_t info;
        int layer = sc->actual_layer[b];
        double start = 0.0, end = 0.0;
        int present = sc->is_present[b];
        if (db_shelf_find(sc->db, sc->book_epc[b], &prev) == 1) {
            layer = prev.layer;
            start = prev.start_cm;
            end = prev.end_cm;
            present = prev.is_present;
        } else if (db_book_find(sc->db, sc->book_epc[b], &info) == 1) {
            layer = info.expected_layer;
            start = info.expected_start;
            end = info.expected_end;
            present = 0;
        }
        db_shelf_upsert(sc->db,
                        sc->book_epc[b], sc->book_title[b], "",
                        layer, start, end,
                        sc->last_rssi[b], present);
    }

    sc->ready = 0;
    printf("[scanner] stopped, %d rounds, shelf_state updated\n", sc->round_count);
}

int scanner_last_seen_sec(scanner_t *sc, const char *epc) {
    int idx = book_index(sc, epc);
    if (idx < 0) return -1;
    return (int)((uint32_t)time(NULL) - sc->last_seen[idx]);
}

void scanner_dump(scanner_t *sc) {
    if (!sc) return;
    printf("[scanner] round=%d ready=%d books=%d events=%d\n",
           sc->round_count, sc->ready, sc->book_count, sc->event_count);

    for (int i = 0; i < 2; i++) {
        printf("  dev[%d] fd=%d scanning=%d tags=%d power=%ddBm\n",
               i, sc->dev[i].fd, sc->dev[i].scanning,
               sc->dev[i].tag_count, sc->power[i]);
    }

    if (sc->book_count > 0) {
        printf("  books:\n");
        uint32_t now = (uint32_t)time(NULL);
        for (int b = 0; b < sc->book_count; b++) {
            int age = sc->last_seen[b] ? (int)(now - sc->last_seen[b]) : -1;
            printf("    %-38s L%d/%d %s age=%ds score=%d/%d\n",
                   sc->book_epc[b], sc->book_layer[b],
                   sc->actual_layer[b],
                   sc->is_present[b] ? "✓" : "✗", age,
                   sc->layer_score[b][0], sc->layer_score[b][1]);
        }
    }

    for (int e = 0; e < sc->event_count; e++) {
        scan_event_t *ev = &sc->events[e];
        printf("  event: type=%d epc=%s title=%s layer=%d rssi=%d\n",
               ev->type, ev->epc, ev->title, ev->layer, ev->rssi);
    }
}
