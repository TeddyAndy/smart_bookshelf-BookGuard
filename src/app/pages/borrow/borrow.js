const app = getApp()
const api = require('../../utils/api')

Page({
  data: {
    // ── 借阅记录 (云端) ──────────────────────────────────
    borrowRecords: [],
    hasRecords: false,

    // ── 统计数据 (云端 + MQTT 混合) ─────────────────────
    statBorrowed: 0,
    statReturned: 0,
    statMisplaced: 0,
    todayBorrows: 0,
    todayReturns: 0,

    // ── 推荐 (云端) ─────────────────────────────────────
    recommendations: [],
    hasRecommendations: false,

    // ── 热门书籍 (云端) ─────────────────────────────────
    popularBooks: [],
    hasPopular: false,

    // ── 盘点状态 ────────────────────────────────────────
    scanning: false,

    // ── 书架概览 ────────────────────────────────────────
    shelfBooks: 0,
    shelfLayers: 0,

    // ── 云端状态 ────────────────────────────────────────
    cloudOnline: false
  },

  onLoad() {
    this._refresh()
    this._unsub = app.onDataChange(() => this._syncMqttData())
  },

  onUnload() {
    if (this._unsub) this._unsub()
  },

  onShow() {
    this._refresh()
  },

  /** 全量刷新 */
  _refresh() {
    this._syncMqttData()
    this._fetchCloudData()
    this.setData({ cloudOnline: app.globalData.cloudStatus === 'online' })
  },

  // ══════════════════════════════════════════════════════════
  // MQTT 实时数据
  // ══════════════════════════════════════════════════════════

  _syncMqttData() {
    const events = app.globalData.events || []
    this.setData({
      statBorrowed: events.filter(e => e.type === 'borrowed').length,
      statReturned: events.filter(e => e.type === 'returned').length,
      statMisplaced: events.filter(e => e.type === 'misplaced').length
    })
    this._buildShelfStats()
  },

  _buildShelfStats() {
    const shelf = app.globalData.shelfData
    if (!shelf) return
    const layerCount = Array.isArray(shelf.layers)
      ? shelf.layers.length
      : Array.isArray(shelf.books)
        ? new Set(shelf.books.map(b => b.layer).filter(v => v != null)).size || 0
        : 0
    this.setData({
      shelfBooks: shelf.total_books || 0,
      shelfLayers: layerCount
    })
  },

  // ══════════════════════════════════════════════════════════
  // 云端数据
  // ══════════════════════════════════════════════════════════

  _fetchCloudData() {
    // 最近借阅事件
    api.getEventsRecent(30).then(rows => {
      const records = (rows || []).map(e => ({
        type: e.event_type,
        typeText: this._typeText(e.event_type),
        typeColor: this._typeColor(e.event_type),
        title: e.title || e.epc?.slice(-8) || '未知',
        epc: e.epc,
        layer: e.layer != null ? (e.layer === 0 ? '上层' : '下层') : '',
        time: api.formatTime(e.occurred_at)
      }))
      this.setData({
        borrowRecords: records,
        hasRecords: records.length > 0
      })
    }).catch(() => {})

    // 今日统计
    api.getDailyStats().then(d => {
      if (!d) return
      this.setData({
        todayBorrows: d.today_borrows || 0,
        todayReturns: d.today_returns || 0
      })
    }).catch(() => {})

    // 综合推荐
    api.getRecommendForYou(8).then(list => {
      if (!list || list.length === 0) {
        this.setData({ recommendations: [], hasRecommendations: false })
        return
      }
      const recs = list.map((b, i) => ({
        ...b,
        reasonLabel: b.reason === 'related' ? '📚 协同推荐' : '🔥 热门好书',
        reasonColor: b.reason === 'related' ? '#722ed1' : '#f5222d',
        rank: i + 1
      }))
      this.setData({
        recommendations: recs,
        hasRecommendations: recs.length > 0
      })
    }).catch(() => {})

    // 热门书籍
    api.getPopularBooks(10).then(list => {
      if (!list || list.length === 0) {
        this.setData({ popularBooks: [], hasPopular: false })
        return
      }
      const books = list.map((b, i) => ({
        ...b,
        rank: i + 1,
        popularityPct: b.popularity ? Math.round(b.popularity * 100) + '%' : '--'
      }))
      this.setData({ popularBooks: books, hasPopular: books.length > 0 })
    }).catch(() => {})
  },

  // ══════════════════════════════════════════════════════════
  // 盘点
  // ══════════════════════════════════════════════════════════

  onScanShelf() {
    const client = app.getMqttClient()
    if (!client || !app.isConnected()) {
      wx.showToast({ title: 'MQTT 未连接，无法盘点', icon: 'none' })
      return
    }

    this.setData({ scanning: true })

    client.publish('bookshelf/cmd/scan', {
      force: true,
      timestamp: new Date().toISOString()
    })

    wx.showToast({ title: '已发送盘点命令', icon: 'success' })

    setTimeout(() => {
      this.setData({ scanning: false })
      this._refresh()
    }, 3000)
  },

  // ══════════════════════════════════════════════════════════
  // 工具
  // ══════════════════════════════════════════════════════════

  _typeText(type) {
    const map = {
      borrow: '借出', return: '归还',
      misplaced: '错放', register: '注册', found: '找到'
    }
    return map[type] || type
  },

  _typeColor(type) {
    const map = {
      borrow: '#f5222d', return: '#52c41a',
      misplaced: '#faad14', register: '#2d8cf0', found: '#722ed1'
    }
    return map[type] || '#999'
  }
})
