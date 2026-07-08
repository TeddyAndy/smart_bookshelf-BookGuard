const app = getApp()
const api = require('../../utils/api')
const config = require('../../utils/config')

Page({
  data: {
    // ── 用户信息 ────────────────────────────────────────
    isLoggedIn: false,
    avatarUrl: '',
    nickName: '访客',

    // ── 个人统计 (MQTT) ────────────────────────────────
    totalBorrows: 0,
    totalReturns: 0,
    favoriteAuthor: '--',

    // ── 今日数据 (云端) ────────────────────────────────
    todayBorrows: '--',
    todayReturns: '--',

    // ── 系统状态 ────────────────────────────────────────
    mqttStatus: '未连接',
    mqttStatusColor: '#999',
    brokerInfo: config.brokerLabel,

    // ── 云端服务状态 ────────────────────────────────────
    cloudStatus: '检测中...',
    cloudStatusColor: '#faad14',
    cloudVersion: '',

    // ── 应用信息 ────────────────────────────────────────
    appVersion: '0.4.5',
    cacheSize: '计算中...'
  },

  onLoad() {
    this._loadUserInfo()
    this._calcCacheSize()
    this._syncSystem()
    this._checkCloud()
    this._unsub = app.onDataChange(() => this._syncSystem())
  },

  onUnload() {
    if (this._unsub) this._unsub()
  },

  onShow() {
    this._syncSystem()
    this._calcCacheSize()
    this._checkCloud()
  },

  // ══════════════════════════════════════════════════════════
  // 用户
  // ══════════════════════════════════════════════════════════

  _loadUserInfo() {
    try {
      const info = wx.getStorageSync('userInfo')
      if (info) {
        this.setData({
          isLoggedIn: true,
          avatarUrl: info.avatarUrl || '',
          nickName: info.nickName || '用户'
        })
      }
    } catch(e) {}
  },

  onGetUserInfo(e) {
    if (e.detail && e.detail.userInfo) {
      const { avatarUrl, nickName } = e.detail.userInfo
      this.setData({
        isLoggedIn: true,
        avatarUrl: avatarUrl,
        nickName: nickName
      })
      wx.setStorageSync('userInfo', { avatarUrl, nickName })
      wx.showToast({ title: '登录成功', icon: 'success' })
    }
  },

  onLogout() {
    wx.showModal({
      title: '退出登录',
      content: '确定要退出吗？',
      success: (res) => {
        if (res.confirm) {
          wx.removeStorageSync('userInfo')
          this.setData({
            isLoggedIn: false,
            avatarUrl: '',
            nickName: '访客'
          })
        }
      }
    })
  },

  // ══════════════════════════════════════════════════════════
  // 系统状态
  // ══════════════════════════════════════════════════════════

  _syncSystem() {
    const g = app.globalData

    const statusMap = {
      connecting: { text: '连接中...', color: '#faad14' },
      active:      { text: '已连接', color: '#52c41a' },
      offline:     { text: '已断开', color: '#ff4d4f' },
      sleep:       { text: '休眠', color: '#faad14' }
    }
    const s = statusMap[g.deviceStatus?.status] || { text: '未连接', color: '#999' }

    const events = g.events || []
    const totalBorrows = events.filter(e => e.type === 'borrowed').length
    const totalReturns = events.filter(e => e.type === 'returned').length

    const authorCount = {}
    events.forEach(e => {
      if (e.author) authorCount[e.author] = (authorCount[e.author] || 0) + 1
    })
    let favoriteAuthor = '--'
    let maxCount = 0
    for (const [author, count] of Object.entries(authorCount)) {
      if (count > maxCount) { maxCount = count; favoriteAuthor = author }
    }

    // 今日数据优先使用 globalData 预加载的
    const ds = g.dailyStats
    const todayB = ds ? ds.today_borrows : '--'
    const todayR = ds ? ds.today_returns : '--'

    this.setData({
      mqttStatus: s.text,
      mqttStatusColor: s.color,
      totalBorrows: totalBorrows,
      totalReturns: totalReturns,
      favoriteAuthor: favoriteAuthor,
      todayBorrows: todayB,
      todayReturns: todayR
    })
  },

  _checkCloud() {
    api.getHealth()
      .then(data => {
        this.setData({
          cloudStatus: '在线 v' + (data.version || '?'),
          cloudStatusColor: '#52c41a',
          cloudVersion: data.version || ''
        })
      })
      .catch(() => {
        this.setData({
          cloudStatus: '不可达',
          cloudStatusColor: '#ff4d4f',
          cloudVersion: ''
        })
      })

    // 今日统计
    api.getDailyStats().then(d => {
      if (!d) return
      this.setData({
        todayBorrows: d.today_borrows || 0,
        todayReturns: d.today_returns || 0
      })
    }).catch(() => {})
  },

  // ══════════════════════════════════════════════════════════
  // 缓存
  // ══════════════════════════════════════════════════════════

  _calcCacheSize() {
    try {
      const info = wx.getStorageInfoSync()
      const kb = (info.currentSize || 0).toFixed(1)
      const limit = (info.limitSize || 0) / 1024
      this.setData({ cacheSize: `${kb}KB / ${limit.toFixed(0)}MB` })
    } catch(e) {
      this.setData({ cacheSize: '未知' })
    }
  },

  onClearCache() {
    wx.showModal({
      title: '清除缓存',
      content: '将清除所有本地数据（包括登录信息），确定继续？',
      success: (res) => {
        if (res.confirm) {
          wx.clearStorageSync()
          this.setData({
            isLoggedIn: false,
            avatarUrl: '',
            nickName: '访客',
            cacheSize: '0KB'
          })
          wx.showToast({ title: '缓存已清除', icon: 'success' })
        }
      }
    })
  },

  // ══════════════════════════════════════════════════════════
  // 操作
  // ══════════════════════════════════════════════════════════

  onCopyBroker() {
    wx.setClipboardData({
      data: config.brokerLabel,
      success: () => wx.showToast({ title: '已复制 Broker 地址', icon: 'success' })
    })
  },

  onOpenDashboard() {
    wx.setClipboardData({
      data: config.apiBase + '/',
      success: () => wx.showToast({ title: 'Web 仪表盘地址已复制，请在浏览器打开', icon: 'none', duration: 2000 })
    })
  },

  onOpenEmqx() {
    wx.setClipboardData({
      data: config.apiBase + ':18083',
      success: () => wx.showToast({ title: 'EMQX Dashboard 地址已复制', icon: 'none', duration: 2000 })
    })
  },

  onScanCode() {
    wx.scanCode({
      onlyFromCamera: true,
      success: (res) => {
        wx.showToast({ title: '扫描结果: ' + res.result, icon: 'none', duration: 2500 })
      },
      fail: () => {}
    })
  }
})
