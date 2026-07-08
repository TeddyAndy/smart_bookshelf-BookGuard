const app = getApp()
const api = require('../../utils/api')

Page({
  data: {
    query: '',
    results: [],
    searched: false,
    searching: false,
    foundBook: null,
    totalBooks: 0,
    resultHint: '',
    selectedIndex: -1,
    locating: false,
    commandSent: false,
    commandTarget: '',

    // ── 书籍历史 (云端) ──────────────────────────────────
    bookHistory: [],
    hasHistory: false,
    historyExpanded: false,

    // ── 关联推荐 (云端) ─────────────────────────────────
    relatedBooks: [],
    hasRelated: false,

    // ── 搜索结果来源 ────────────────────────────────────
    cloudResults: false
  },

  onLoad() {
    this._syncShelf()
    this._unsub = app.onDataChange(() => {
      if (!this.data.searched) {
        this._syncShelf()
      }
    })
  },

  onUnload() {
    if (this._unsub) this._unsub()
  },

  onShow() {
    this._syncShelf()
  },

  // ══════════════════════════════════════════════════════════
  // 本地书架数据
  // ══════════════════════════════════════════════════════════

  _syncShelf() {
    const shelf = app.globalData.shelfData
    if (!shelf) {
      this.setData({ totalBooks: 0 })
      return
    }

    const allBooks = []
    const layers = Array.isArray(shelf.layers) ? shelf.layers : null
    const books = Array.isArray(shelf.books) ? shelf.books : null

    if (layers) {
      layers.forEach(layer => {
        if (Array.isArray(layer.books)) {
          layer.books.filter(b => b && b.epc).forEach(b => {
            allBooks.push({
              ...b,
              start_cm: b.start_cm != null ? b.start_cm : (b.pos_cm != null ? b.pos_cm : 0),
              end_cm: b.end_cm != null ? b.end_cm : ((b.pos_cm != null ? b.pos_cm : 0) + 2),
              layerName: layer.name || ('第' + ((layer.index || 0) + 1) + '层'),
              layerIndex: layer.index
            })
          })
        }
      })
    } else if (books) {
      books.filter(b => b && b.epc).forEach(b => {
        const layerIndex = b.layer != null ? b.layer : 0
        const start = b.start_cm != null ? b.start_cm : (b.pos_cm != null ? b.pos_cm : 0)
        const end = b.end_cm != null ? b.end_cm : (start + 2)
        allBooks.push({
          ...b,
          start_cm: start,
          end_cm: end,
          layerName: layerIndex === 0 ? '上层' : '下层',
          layerIndex: layerIndex
        })
      })
    }

    this.setData({
      totalBooks: allBooks.length,
      _allBooks: allBooks
    })
  },

  // ══════════════════════════════════════════════════════════
  // 搜索
  // ══════════════════════════════════════════════════════════

  onInput(e) {
    this.setData({ query: e.detail.value })
  },

  onSearch() {
    const query = this.data.query.trim()
    if (!query) {
      wx.showToast({ title: '请输入书名或编号', icon: 'none' })
      return
    }

    this.setData({
      searching: true,
      searched: true,
      foundBook: null,
      resultHint: '',
      selectedIndex: -1,
      commandSent: false,
      commandTarget: ''
    })

    const allBooks = this.data._allBooks || []
    const results = this._localSearch(query, allBooks)

    if (results.length > 0) {
      if (results.length === 1) {
        this._openBook(results[0], 0, {
          results,
          searching: false,
          resultHint: '已为你定位到 1 本匹配图书，确认后可发送灯光查找。'
        })
      } else {
        const exact = this._pickBestMatch(query, results)
        if (exact) {
          const exactIndex = results.findIndex(item => item.epc === exact.epc)
          this._openBook(exact, exactIndex, {
            results,
            searching: false,
            resultHint: `找到 ${results.length} 本相关图书，已优先选中最接近的一本。`
          })
        } else {
          this.setData({
            results,
            foundBook: null,
            searching: false,
            resultHint: `找到 ${results.length} 本相关图书，请先点选目标，再发送灯光查找。`
          })
        }
      }
    } else {
      this._cloudSearch(query)
    }
  },

  /** 云端模糊搜索 */
  _cloudSearch(query) {
    api.searchBooks(query, 20).then(list => {
      if (list && list.length > 0) {
        const results = list.map(b => ({
          epc: b.epc,
          title: b.title,
          author: '',
          layerName: '云端数据库',
          layerIndex: -1,
          rssi: null,
          present: true
        }))
        if (results.length === 1) {
          this._openBook(results[0], 0, {
            results,
            searching: false,
            cloudResults: true,
            resultHint: '云端匹配到 1 本图书。若要灯光引导，请先确认这本书就是目标。'
          })
        } else {
          this.setData({
            results,
            foundBook: null,
            searching: false,
            cloudResults: true,
            resultHint: `云端找到 ${list.length} 本相关图书，请先选择具体书籍。`
          })
        }
        wx.showToast({ title: '已生成候选结果', icon: 'none' })
      } else {
        this.setData({
          results: [],
          foundBook: null,
          searching: false,
          cloudResults: false,
          resultHint: '没有找到明确结果。你可以换更完整的书名，或使用准确 EPC。'
        })
        wx.showToast({ title: '未找到匹配图书', icon: 'none', duration: 2000 })
      }
    }).catch(() => {
      this.setData({
        results: [],
        foundBook: null,
        searching: false,
        cloudResults: false,
        resultHint: '云端搜索暂时不可用，请稍后重试。'
      })
      wx.showToast({ title: '云端搜索失败', icon: 'none', duration: 2000 })
    })
  },

  _localSearch(query, books) {
    const q = query.toLowerCase()
    return books.filter(b => {
      if (b.epc && b.epc.toLowerCase().includes(q)) return true
      if (b.title && b.title.toLowerCase().includes(q)) return true
      if (b.author && b.author.toLowerCase().includes(q)) return true
      return false
    })
  },

  _sendFindCmd(query) {
    const client = app.getMqttClient()
    if (!client || !app.isConnected()) {
      console.warn('[搜索] MQTT 未连接，无法发送查找命令')
      return
    }
    client.publish('bookshelf/cmd/find', {
      query: query,
      timestamp: new Date().toISOString()
    })
    console.log('[搜索] 已发送查找命令:', query)
  },

  // ══════════════════════════════════════════════════════════
  // 选择书籍 → 拉取云端历史 + 关联推荐
  // ══════════════════════════════════════════════════════════

  onSelectResult(e) {
    const index = e.currentTarget.dataset.index
    const book = this.data.results[index]
    if (book) {
      this._openBook(book, index)
    }
  },

  onSelectBrowse(e) {
    const index = e.currentTarget.dataset.index
    const book = (this.data._allBooks || [])[index]
    if (book) {
      this._openBook(book, index, {
        searched: true,
        results: [book],
        resultHint: '已选中图书，确认后可发送灯光查找。'
      })
    }
  },

  onLocateBook() {
    const book = this.data.foundBook
    if (!book) return

    const client = app.getMqttClient()
    if (!client || !app.isConnected()) {
      wx.showToast({ title: 'MQTT 未连接，无法发送', icon: 'none' })
      return
    }

    this.setData({ locating: true })
    const query = book.title || book.epc
    client.publish('bookshelf/cmd/find', {
      query,
      epc: book.epc,
      timestamp: new Date().toISOString()
    })
    this.setData({
      locating: false,
      commandSent: true,
      commandTarget: query
    })
    wx.showToast({ title: '已发送灯光查找', icon: 'success' })
  },

  _fetchBookDetails(epc) {
    if (!epc) return

    // 借阅历史
    api.getBookHistory(epc, 10).then(rows => {
      const history = (rows || []).map(e => ({
        type: e.event_type,
        typeText: this._typeText(e.event_type),
        time: api.formatTime(e.occurred_at),
        layer: e.layer != null ? (e.layer === 0 ? '上层' : '下层') : ''
      }))
      this.setData({
        bookHistory: history,
        hasHistory: history.length > 0,
        historyExpanded: false
      })
    }).catch(() => {})

    // 关联推荐
    api.getRelatedBooks(epc, 6).then(list => {
      if (!list || list.length === 0) {
        this.setData({ relatedBooks: [], hasRelated: false })
        return
      }
      this.setData({
        relatedBooks: list.map(b => ({
          ...b,
          coPctLabel: b.co_pct != null ? b.co_pct + '%' : ''
        })),
        hasRelated: true
      })
    }).catch(() => {})
  },

  /** 返回搜索结果列表 */
  onBackToList() {
    this.setData({ foundBook: null, commandSent: false, commandTarget: '' })
  },

  onToggleHistory() {
    if (!this.data.hasHistory) return
    this.setData({ historyExpanded: !this.data.historyExpanded })
  },

  onClear() {
    this.setData({
      query: '',
      results: [],
      searched: false,
      foundBook: null,
      resultHint: '',
      selectedIndex: -1,
      locating: false,
      commandSent: false,
      commandTarget: '',
      bookHistory: [],
      hasHistory: false,
      historyExpanded: false,
      relatedBooks: [],
      hasRelated: false,
      cloudResults: false
    })
  },

  // ══════════════════════════════════════════════════════════
  // 工具
  // ══════════════════════════════════════════════════════════

  _formatPosition(book) {
    if (book.layerIndex != null && book.start_cm != null) {
      return `${book.layerName} ${book.start_cm}-${book.end_cm}cm`
    }
    return '位置未知'
  },

  _typeText(type) {
    const map = {
      borrow: '借出', return: '归还',
      misplaced: '错放', register: '注册', found: '找到'
    }
    return map[type] || type
  },

  _pickBestMatch(query, results) {
    const normalized = query.toLowerCase()
    return results.find(b => (b.title || '').toLowerCase() === normalized)
      || results.find(b => (b.epc || '').toLowerCase() === normalized)
      || null
  },

  _openBook(book, index, extra = {}) {
    this.setData({
      foundBook: book,
      selectedIndex: index,
      commandSent: false,
      commandTarget: '',
      ...extra
    })
    this._fetchBookDetails(book.epc)
    wx.pageScrollTo({ scrollTop: 0 })
  }
})
