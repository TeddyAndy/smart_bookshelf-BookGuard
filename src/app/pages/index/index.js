const app = getApp()
const api = require('../../utils/api')

Page({
  data: {
    // ── MQTT 实时环境 ────────────────────────────────────
    temperature: '--',
    humidity: '--',
    lux: '--',
    radarState: null,
    radarMove: null,
    radarStill: null,

    // ── 设备状态 ──────────────────────────────────────────
    mqttStatus: 'connecting',
    statusText: '连接中...',
    statusColor: '#faad14',
    deviceState: '--',

    // ── 告警 ──────────────────────────────────────────────
    alarmList: [],

    // ── 书架概览 ──────────────────────────────────────────
    totalBooks: '--',
    totalSlots: '--',

    // ── 最近事件 ──────────────────────────────────────────
    recentEvents: [],

    // ── 云端 24h 趋势 ─────────────────────────────────────
    trendLabels: [],     // 时间标签 ["08:00","09:00",...]
    trendTemps: [],      // 温度序列
    trendHums: [],       // 湿度序列
    trendLux: [],        // 光照序列
    trendTempMin: 0,
    trendTempMax: 0,
    trendHumMin: 0,
    trendHumMax: 0,
    trendLuxMin: 0,
    trendLuxMax: 0,
    chartWidth: 0,
    chartHeight: 160,
    hasTrend: false,

    // ── 云端环境摘要 ─────────────────────────────────────
    summary: null,
    suggestions: [],

    // ── 云端服务状态 ─────────────────────────────────────
    cloudOnline: false
  },

  _unsubDataChange: null,

  onLoad() {
    this._initChartSize()
    this._syncFromApp()
    this._fetchCloudData()
    this._unsubDataChange = app.onDataChange(() => this._syncFromApp())
  },

  onShow() {
    // 每次显示时刷新云端数据
    if (this.data.hasTrend) this._drawTrendCharts()
    this._fetchCloudData()
  },

  onUnload() {
    if (this._unsubDataChange) this._unsubDataChange()
  },

  /** 拉取云端数据：24h 曲线 + 环境摘要 */
  _fetchCloudData() {
    this._fetchLatestFallback()
    this._fetchShelfFallback()
    this._fetchRecentEventsFallback()

    // 云端 24h 曲线
    api.getSensors24h().then(rows => {
      if (!rows || rows.length === 0) return
      const labels = [], temps = [], hums = [], luxs = []
      rows.forEach(r => {
        const t = api.formatTime(r.recorded_at)
        labels.push(t.slice(-5))  // 只取 HH:MM
        temps.push(r.temp_c)
        hums.push(r.hum_pct)
        luxs.push(r.lux)
      })
      const tempStats = this._calcRange(temps)
      const humStats = this._calcRange(hums)
      const luxStats = this._calcRange(luxs)
      this.setData({
        trendLabels: labels,
        trendTemps: temps,
        trendHums: hums,
        trendLux: luxs,
        trendTempMin: tempStats.min,
        trendTempMax: tempStats.max,
        trendHumMin: humStats.min,
        trendHumMax: humStats.max,
        trendLuxMin: luxStats.min,
        trendLuxMax: luxStats.max,
        hasTrend: rows.length > 0
      }, () => this._drawTrendCharts())
    }).catch(() => {})

    // 7 天环境摘要
    api.getSensorsSummary().then(s => {
      if (!s || s.days === 0) return
      this.setData({
        summary: s,
        suggestions: s.suggestions || []
      })
    }).catch(() => {})

    // 云端健康
    this.setData({ cloudOnline: app.globalData.cloudStatus === 'online' })
  },

  _initChartSize() {
    const windowInfo = typeof wx.getWindowInfo === 'function'
      ? wx.getWindowInfo()
      : wx.getSystemInfoSync()
    const width = Math.max(Math.floor(windowInfo.windowWidth - 64), 260)
    this.setData({ chartWidth: width })
  },

  _fetchLatestFallback() {
    const sensor = app.globalData.sensorData || {}
    if (sensor.temperature !== '--' && sensor.humidity !== '--' && sensor.lux !== '--') return

    api.getSensorsLatest().then(row => {
      if (!row || row.temp_c == null) return
      app.globalData.sensorData = {
        temperature: `${Number(row.temp_c).toFixed(1)}°C`,
        humidity: `${Math.round(Number(row.hum_pct || 0))}%`,
        lux: `${Math.round(Number(row.lux || 0))} lx`,
        radar: null
      }
      app.globalData.deviceStatus = {
        ...(app.globalData.deviceStatus || {}),
        lastSeen: row.recorded_at || new Date().toLocaleString()
      }
      this._syncFromApp()
    }).catch(() => {})
  },

  _fetchShelfFallback() {
    if (app.globalData.shelfData) return

    api.getShelfCurrent().then(row => {
      if (!row) return
      app.globalData.shelfData = row
      this._syncFromApp()
    }).catch(() => {})
  },

  _fetchRecentEventsFallback() {
    if ((app.globalData.events || []).length > 0) return

    api.getEventsRecent(3).then(rows => {
      if (!rows || rows.length === 0) return
      app.globalData.events = rows.map((item, index) => ({
        id: `${item.occurred_at || Date.now()}_${index}`,
        type: item.event_type,
        title: item.title,
        epc: item.epc,
        timestamp: item.occurred_at
      }))
      this._syncFromApp()
    }).catch(() => {})
  },

  _calcRange(list) {
    const values = list.filter(v => typeof v === 'number' && !isNaN(v))
    if (values.length === 0) return { min: 0, max: 0 }

    let min = values[0]
    let max = values[0]
    values.forEach(v => {
      if (v < min) min = v
      if (v > max) max = v
    })
    return { min, max }
  },

  /** 从 app.globalData 同步数据到页面 */
  _syncFromApp() {
    const g = app.globalData

    // 连接状态
    const statusMap = {
      connecting: { text: '连接中...', color: '#faad14' },
      active:      { text: '设备在线', color: '#52c41a' },
      offline:     { text: '设备离线', color: '#ff4d4f' },
      sleep:       { text: '设备休眠', color: '#faad14' }
    }
    const s = statusMap[g.deviceStatus?.status] || statusMap.connecting
    const mqttStatus = g.deviceStatus?.status || 'connecting'

    // 传感器数据
    const sensor = g.sensorData || {}
    const radar = sensor.radar

    // 告警逻辑（基于真实传感器数据）
    const alarms = []
    if (g.lastError) {
      alarms.push(`🔧 设备故障: ${g.lastError.message}`)
    }
    const temp = parseFloat(sensor.temperature)
    if (!isNaN(temp) && temp > 35) {
      alarms.push(`🔥 温度偏高: ${temp.toFixed(1)}°C`)
    }

    // 书架数据
    const shelf = g.shelfData
    const totalBooks = shelf ? shelf.total_books : '--'
    const totalSlots = shelf ? shelf.total_slots : '--'

    // 书架可视化数据
    const shelfLayers = this._buildShelfLayers(shelf)

    // 最近事件 (取最近 3 条)
    const events = (g.events || []).slice(0, 3)
    const recentEvents = events.map(e => ({
      ...e,
      typeText: this._eventTypeText(e.type),
      time: this._formatTime(e.timestamp)
    }))

    this.setData({
      temperature: sensor.temperature || '--',
      humidity: sensor.humidity || '--',
      lux: sensor.lux || '--',
      radarState: radar ? radar.state : null,
      radarMove: radar ? radar.move_dist_cm : null,
      radarStill: radar ? radar.still_dist_cm : null,
      mqttStatus: mqttStatus,
      statusText: s.text,
      statusColor: s.color,
      alarmList: alarms,
      totalBooks: totalBooks,
      totalSlots: totalSlots,
      shelfLayers: shelfLayers,
      hasShelfData: shelfLayers.length > 0,
      recentEvents: recentEvents,
      deviceState: g.deviceState && g.deviceState.state ? g.deviceState.state : '--'
    })
  },

  _eventTypeText(type) {
    const map = {
      borrowed:   '📤 借出',
      returned:   '📥 归还',
      misplaced:  '⚠️ 错放',
      registered: '📝 注册',
      found:      '🔍 找到'
    }
    return map[type] || type || '--'
  },

  _formatTime(ts) {
    if (!ts) return '--'
    try {
      // 如果已经是格式化字符串
      if (typeof ts === 'string' && ts.includes('-')) return ts.slice(5, 16).replace(' ', ' ')
      // Unix 时间戳
      const d = new Date(ts)
      const pad = n => String(n).padStart(2, '0')
      return `${d.getMonth()+1}/${d.getDate()} ${pad(d.getHours())}:${pad(d.getMinutes())}`
    } catch(e) {
      return '--'
    }
  },

  /** 构建书架可视化 — 精准位置竖排书脊 */
  _buildShelfLayers(shelf) {
    if (!shelf || !shelf.layers) return []

    const BOOK_COLORS = [
      '#2d8cf0', '#52c41a', '#fa8c16', '#eb2f96',
      '#722ed1', '#13c2c2', '#f5222d', '#faad14',
      '#1890ff', '#a0d911', '#f759ab', '#2f54eb'
    ]
    const LAYER_WIDTH = 38  // cm

    return shelf.layers.map(layer => {
      const rawBooks = (layer.books || []).filter(b => b.present || b.epc)

      // 合并相邻同一本书，构建精确定位的书脊
      const spines = []
      let colorIdx = 0
      for (const b of rawBooks) {
        if (!b.epc) continue
        const start = b.start_cm || 0
        const end = b.end_cm || 0
        const thick = end - start
        if (thick < 0.3) continue

        const name = (b.title || b.epc.slice(-8) || '?').slice(0, 8)
        spines.push({
          epc: b.epc,
          title: name,
          color: BOOK_COLORS[colorIdx % BOOK_COLORS.length],
          leftPct: (start / LAYER_WIDTH * 100).toFixed(1),   // 左边距%
          widthPct: Math.max((thick / LAYER_WIDTH * 100).toFixed(1), 1.5), // 宽度%
          start_cm: start,
          end_cm: end,
          rssi: b.rssi
        })
        colorIdx++
      }

      spines.sort((a, b) => a.start_cm - b.start_cm)

      return {
        name: layer.name || ('层' + (layer.index + 1)),
        index: layer.index,
        layerWidth: LAYER_WIDTH,
        spines: spines,
        bookCount: spines.length
      }
    })
  },

  _drawTrendCharts() {
    if (!this.data.hasTrend || !this.data.chartWidth) return

    this._drawLineChart('tempChart', this.data.trendTemps, {
      width: this.data.chartWidth,
      height: this.data.chartHeight,
      color: '#f25f5c',
      fillColor: 'rgba(242, 95, 92, 0.16)',
      baselineColor: '#fbd4d3',
      labels: this.data.trendLabels,
      unit: '°C',
      min: this.data.trendTempMin,
      max: this.data.trendTempMax
    })

    this._drawLineChart('humChart', this.data.trendHums, {
      width: this.data.chartWidth,
      height: this.data.chartHeight,
      color: '#3f8cff',
      fillColor: 'rgba(63, 140, 255, 0.15)',
      baselineColor: '#d8e9ff',
      labels: this.data.trendLabels,
      unit: '%',
      min: this.data.trendHumMin,
      max: this.data.trendHumMax
    })

    this._drawLineChart('luxChart', this.data.trendLux, {
      width: this.data.chartWidth,
      height: this.data.chartHeight,
      color: '#f6a623',
      fillColor: 'rgba(246, 166, 35, 0.18)',
      baselineColor: '#fde8be',
      labels: this.data.trendLabels,
      unit: 'lx',
      min: this.data.trendLuxMin,
      max: this.data.trendLuxMax
    })
  },

  _drawLineChart(canvasId, values, options) {
    if (!values || values.length === 0) return

    const ctx = wx.createCanvasContext(canvasId, this)
    const width = options.width
    const height = options.height
    const padding = { top: 16, right: 12, bottom: 24, left: 44 }
    const innerWidth = width - padding.left - padding.right
    const innerHeight = height - padding.top - padding.bottom
    const min = options.min
    const max = options.max
    const span = Math.max(max - min, 1)
    const lastIndex = Math.max(values.length - 1, 1)

    ctx.clearRect(0, 0, width, height)
    ctx.setFillStyle('#ffffff')
    ctx.fillRect(0, 0, width, height)

    const axisValues = [max, min + span / 2, min]
    for (let i = 0; i < 3; i++) {
      const y = padding.top + innerHeight * (i / 2)
      ctx.setStrokeStyle(i === 2 ? options.baselineColor : '#edf1f5')
      ctx.setLineWidth(i === 2 ? 1.2 : 1)
      ctx.beginPath()
      ctx.moveTo(padding.left, y)
      ctx.lineTo(width - padding.right, y)
      ctx.stroke()

      ctx.setFillStyle('#9aa4b2')
      ctx.setFontSize(9)
      ctx.setTextAlign('left')
      ctx.fillText(this._formatAxisValue(axisValues[i], options.unit), 2, y + 3)
    }

    const points = values.map((value, index) => {
      const x = padding.left + innerWidth * (index / lastIndex)
      const ratio = (value - min) / span
      const y = padding.top + innerHeight - ratio * innerHeight
      return { x, y, value }
    })

    ctx.beginPath()
    ctx.moveTo(points[0].x, padding.top + innerHeight)
    points.forEach((point) => ctx.lineTo(point.x, point.y))
    ctx.lineTo(points[points.length - 1].x, padding.top + innerHeight)
    ctx.closePath()
    ctx.setFillStyle(options.fillColor)
    ctx.fill()

    ctx.beginPath()
    points.forEach((point, index) => {
      if (index === 0) ctx.moveTo(point.x, point.y)
      else ctx.lineTo(point.x, point.y)
    })
    ctx.setStrokeStyle(options.color)
    ctx.setLineWidth(2)
    ctx.stroke()

    const focusPoints = [0, Math.floor(lastIndex / 2), lastIndex]
    focusPoints.forEach(index => {
      const point = points[index]
      const label = options.labels[index] || ''
      ctx.setFillStyle(options.color)
      ctx.beginPath()
      ctx.arc(point.x, point.y, 2.5, 0, Math.PI * 2)
      ctx.fill()
      ctx.setFillStyle('#a0a8b8')
      ctx.setFontSize(9)
      ctx.setTextAlign(index === lastIndex ? 'right' : index === 0 ? 'left' : 'center')
      ctx.fillText(label, point.x, height - 6)
    })

    ctx.draw()
  },

  _formatAxisValue(value, unit) {
    const num = Number(value)
    if (isNaN(num)) return '--'
    if (unit === 'lx') return `${Math.round(num)}`
    return `${num.toFixed(1)}`
  }
})
