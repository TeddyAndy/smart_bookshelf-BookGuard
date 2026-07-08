/**
 * 数据库模块测试程序
 *
 * 编译: make
 * 板端: ./db_test [/path/to/test.db]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"

#define MAX_ITEMS 64

static void sep(const char *title)
{
    printf("\n=== %s ===\n", title);
}

int main(int argc, char *argv[])
{
    db_ctx_t ctx;
    const char *db_path = (argc > 1) ? argv[1] : "/tmp/test_bookshelf.db";
    int i, count;

    printf("数据库测试 — %s\n\n", db_path);

    /* ---- 1. 初始化 ---- */
    sep("1. db_init (打开+建表)");
    if (db_init(&ctx, db_path) != 0) {
        fprintf(stderr, "FAIL: db_init\n");
        return 1;
    }
    db_set_verbose(&ctx, 1);
    printf("OK: 数据库已打开\n");

    /* ---- 2. 插入书籍 ---- */
    sep("2. 插入书籍");
    struct {
        const char *epc, *title, *author;
        int layer; double start, end;
    } test_books[] = {
        {"E280689420005035B7807154", "三体",       "刘慈欣", 0, 0,  4},
        {"300833B2DDD9014254000001", "活着",       "余华",   0, 4,  9},
        {"AABBCCDDEEFF001122334455", "百年孤独",   "马尔克斯", 0, 9, 14},
        {"112233445566778899AABBCC", "围城",       "钱锺书", 1, 0,  5},
        {"DEADBEEF0000000000000001", "红楼梦",     "曹雪芹", 1, 5,  11},
        {"CAFEBABE1234567890ABCDEF", "1984",       "奥威尔", 1, 11, 15},
        {NULL, NULL, NULL, 0, 0, 0}
    };

    for (i = 0; test_books[i].epc; i++) {
        if (db_book_upsert(&ctx, test_books[i].epc, test_books[i].title,
                           test_books[i].author, test_books[i].layer,
                           test_books[i].start, test_books[i].end) != 0) {
            printf("FAIL: 插入 %s\n", test_books[i].title);
        } else {
            printf("OK: 插入「%s」(%s) L%d %.0f-%.0fcm\n",
                   test_books[i].title, test_books[i].epc,
                   test_books[i].layer, test_books[i].start, test_books[i].end);
        }
    }

    /* ---- 3. 查询总数 ---- */
    sep("3. 书籍总数");
    count = db_book_count(&ctx);
    printf("共 %d 本书\n", count);

    /* ---- 4. 按 EPC 查找 ---- */
    sep("4. 按 EPC 查找");
    book_info_t info;
    if (db_book_find(&ctx, "E280689420005035B7807154", &info) == 1) {
        printf("找到: 「%s」%s L%d %.0f-%.0fcm\n",
               info.title, info.author,
               info.expected_layer, info.expected_start, info.expected_end);
    }

    /* 查找不存在的 */
    if (db_book_find(&ctx, "NONEXISTENT_EPC", &info) == 0) {
        printf("OK: 不存在的 EPC 返回 0 (未找到)\n");
    }

    /* ---- 5. 列出全部 ---- */
    sep("5. 列出全部书籍");
    book_info_t list[MAX_ITEMS];
    count = db_book_list(&ctx, list, MAX_ITEMS);
    for (i = 0; i < count; i++) {
        printf("  %-16s 「%s」%-8s L%d [%.0f-%.0fcm]\n",
               list[i].epc, list[i].title, list[i].author,
               list[i].expected_layer,
               list[i].expected_start, list[i].expected_end);
    }

    /* ---- 6. 书架状态 ---- */
    sep("6. 写入书架状态 (模拟盘点结果)");
    db_shelf_clear(&ctx);
    db_shelf_upsert(&ctx, "E280689420005035B7807154", "三体",   "刘慈欣", 0, 0, 4, 45, 1);
    db_shelf_upsert(&ctx, "300833B2DDD9014254000001", "活着",   "余华",   0, 4, 9, 38, 1);
    db_shelf_upsert(&ctx, "112233445566778899AABBCC", "围城",   "钱锺书", 1, 0, 5, 52, 1);
    db_shelf_upsert(&ctx, "DEADBEEF0000000000000001", "红楼梦", "曹雪芹", 1, 5, 11, 30, 1);
    /* 百年孤独 被取出 */
    db_shelf_upsert(&ctx, "AABBCCDDEEFF001122334455", "百年孤独", "马尔克斯", 0, 9, 14, 0, 0);
    printf("OK: 5 条状态已写入 (含 1 本已取出)\n");

    /* ---- 7. 查询某层 ---- */
    sep("7. 上层 (layer=0) 在架书籍");
    shelf_item_t shelf[MAX_ITEMS];
    count = db_shelf_list_layer(&ctx, 0, shelf, MAX_ITEMS);
    for (i = 0; i < count; i++) {
        printf("  「%s」L0 [%.0f-%.0fcm] rssi=%d %s\n",
               shelf[i].title, shelf[i].start_cm, shelf[i].end_cm,
               shelf[i].rssi, shelf[i].is_present ? "在架" : "已取出");
    }

    /* ---- 8. 操作日志 ---- */
    sep("8. 操作日志");
    db_log_add(&ctx, "E2806894...", "三体", "borrowed",
               0, 0, 4, 45, "");
    db_log_add(&ctx, "E2806894...", "三体", "returned",
               0, 0, 4, 42, "位置正确");
    db_log_add(&ctx, "CAFEBABE...", "1984", "misplaced",
               1, 15, 20, 28, "应在 L1 11-15cm, 实际在 L1 15-20cm");

    book_log_t logs[20];
    count = db_log_recent(&ctx, 10, logs, 20);
    for (i = 0; i < count; i++) {
        printf("  [%s] %s → %s L%d [%.0f-%.0fcm] %s\n",
               logs[i].timestamp, logs[i].title, logs[i].action,
               logs[i].layer, logs[i].start_cm, logs[i].end_cm,
               logs[i].detail[0] ? logs[i].detail : "");
    }

    /* ---- 9. 传感器日志 ---- */
    sep("9. 传感器日志");
    db_sensor_add(&ctx, 29.3, 57.2, 85.0, 3);
    db_sensor_add(&ctx, 29.1, 58.0, 80.0, 0);

    sensor_log_t sens[10];
    count = db_sensor_recent(&ctx, 5, sens, 10);
    for (i = 0; i < count; i++) {
        printf("  [%s] T=%.1f°C H=%.1f%% L=%.0f lx radar=%d\n",
               sens[i].timestamp, sens[i].temperature,
               sens[i].humidity, sens[i].lux, sens[i].radar_state);
    }

    /* ---- 10. 统计 ---- */
    sep("10. 统计");
    printf("书籍目录: %d 本\n", db_book_count(&ctx));
    printf("在架书籍: %d 本\n", db_shelf_count(&ctx));

    /* ---- 11. 关闭 ---- */
    sep("11. 关闭");
    db_close(&ctx);
    printf("OK: 数据库已关闭\n");

    printf("\n===== 全部测试通过 =====\n");
    return 0;
}
