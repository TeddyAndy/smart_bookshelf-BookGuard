/** BookGuard 云端 API 工具层 — v0.4.5
 *
 *  封装全部 11 个 HTTP API 端点，统一错误处理和超时控制。
 *  用法: const api = require('../../utils/api')
 *        api.getSensors24h().then(data => ...)
 */

const config = require('./config')

const API_BASE = config.apiBase
const TIMEOUT = 8000 // 8 秒超时

/**
 * 通用 GET 请求
 * @param {string} path  — API 路径 (如 /api/health)
 * @param {object} params — 查询参数 (可选)
 * @returns {Promise<any>} 响应 data
 */
function get(path, params = {}) {
  return new Promise((resolve, reject) => {
    const qs = Object.keys(params).length > 0
      ? '?' + Object.entries(params)
          .filter(([, v]) => v != null)
          .map(([k, v]) => `${k}=${encodeURIComponent(v)}`)
          .join('&')
      : ''

    wx.request({
      url: API_BASE + path + qs,
      method: 'GET',
      timeout: TIMEOUT,
      success(res) {
        if (res.statusCode === 200) {
          resolve(res.data)
        } else {
          console.warn('[API]', path, 'HTTP', res.statusCode)
          reject(new Error(`HTTP ${res.statusCode}: ${path}`))
        }
      },
      fail(err) {
        console.warn('[API]', path, '网络错误:', err.errMsg)
        reject(new Error(`网络请求失败: ${err.errMsg}`))
      }
    })
  })
}

// ════════════════════════════════════════════════════════════
// 基础 API (3)
// ════════════════════════════════════════════════════════════

/** 健康检查 */
function getHealth() {
  return get('/api/health')
}

/** 当前书架快照 */
function getShelfCurrent() {
  return get('/api/shelf/current').then(data => {
    // layers 和 books 是 jsonb 字符串，需解析
    if (data && typeof data.layers === 'string') {
      try { data.layers = JSON.parse(data.layers) } catch (_) {}
    }
    if (data && typeof data.books === 'string') {
      try { data.books = JSON.parse(data.books) } catch (_) {}
    }
    return data
  })
}

/** 最新传感器读数 */
function getSensorsLatest() {
  return get('/api/sensors/latest')
}

// ════════════════════════════════════════════════════════════
// 数据分析 API (5)
// ════════════════════════════════════════════════════════════

/** 最近 24 小时传感器曲线 (每小时采样) */
function getSensors24h() {
  return get('/api/sensors/24h')
}

/** 最近 N 分钟传感器曲线 (每分钟采样，默认 120 分钟) */
function getSensorsRecent(minutes = 120) {
  return get('/api/sensors/recent', { minutes })
}

/** 7 天环境摘要 + 异常检测 + 建议 */
function getSensorsSummary() {
  return get('/api/sensors/summary')
}

/** 最近借阅事件 */
function getEventsRecent(limit = 20) {
  return get('/api/events/recent', { limit })
}

/** 某本书的借阅历史 */
function getBookHistory(epc, limit = 50) {
  return get(`/api/books/${encodeURIComponent(epc)}/history`, { limit })
}

/** 模糊搜索书名/EPC */
function searchBooks(query, limit = 20) {
  return get('/api/books/search', { q: query, limit })
}

/** 热门书籍排行 */
function getPopularBooks(limit = 10) {
  return get('/api/stats/popular', { limit })
}

/** 今日借阅统计 */
function getDailyStats() {
  return get('/api/stats/daily')
}

// ════════════════════════════════════════════════════════════
// 推荐引擎 API (2)
// ════════════════════════════════════════════════════════════

/** 关联推荐 — 借过此书的人也借过哪些 */
function getRelatedBooks(epc, limit = 8) {
  return get(`/api/recommend/related`, { epc, limit })
}

/** 综合推荐 — 基于最近借阅 + 热门补位 */
function getRecommendForYou(limit = 10) {
  return get('/api/recommend/for-you', { limit })
}

// ════════════════════════════════════════════════════════════
// 工具函数
// ════════════════════════════════════════════════════════════

/** 格式化 ISO 时间为可读字符串 */
function formatTime(isoStr) {
  if (!isoStr) return ''
  try {
    const d = new Date(isoStr.replace(' ', 'T'))
    if (isNaN(d.getTime())) return isoStr.slice(0, 16)
    const pad = n => String(n).padStart(2, '0')
    return `${d.getMonth() + 1}/${d.getDate()} ${pad(d.getHours())}:${pad(d.getMinutes())}`
  } catch (_) { return '' }
}

module.exports = {
  API_BASE,
  getHealth,
  getShelfCurrent,
  getSensorsLatest,
  getSensors24h,
  getSensorsRecent,
  getSensorsSummary,
  getEventsRecent,
  getBookHistory,
  searchBooks,
  getPopularBooks,
  getDailyStats,
  getRelatedBooks,
  getRecommendForYou,
  formatTime
}
