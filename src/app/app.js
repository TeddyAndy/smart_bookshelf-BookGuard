const mqtt = require('./utils/mqtt')
const api = require('./utils/api')
const config = require('./utils/config')

// ── MQTT 服务器配置 ─────────────────────────────────────
const MQTT_CONFIG = {
  url: config.mqttUrl,
  options: {
    clientId: 'mini_program_' + Date.now(),
    username: config.mqttUsername,
    password: config.mqttPassword,
    keepAlive: 60,
    cleanSession: true
  }
}

App({
  globalData: {
    // ── MQTT 实时数据 ────────────────────────────────────
    sensorData: { temperature: '--', humidity: '--', lux: '--', radar: null },
    shelfData: null,
    events: [],
    deviceStatus: { status: 'connecting', lastSeen: null },
    mqttClient: null,
    mqttConnected: false,

    // ── 云端 API 状态 ────────────────────────────────────
    cloudStatus: 'unknown',     // unknown | online | offline
    cloudVersion: '',
    dailyStats: null,           // 今日借阅统计
    popularBooks: [],           // 热门书籍
    sensorSummary: null         // 7 天环境摘要
  },

  onLaunch() {
    console.log('[App] 小程序启动，连接 MQTT + 检查云端...')
    this._connectMqtt()
    this.checkCloud()
  },

  onShow() {
    // 从后台恢复时检查连接
    if (!this.globalData.mqttConnected) {
      console.log('[App] 从后台恢复，重新连接 MQTT...')
      this._connectMqtt()
    }
  },

  onHide() {
    // 进入后台时保持连接 (keepAlive 会自动心跳)
    console.log('[App] 进入后台')
  },

  // ── MQTT 连接 ─────────────────────────────────────────

  _connectMqtt() {
    if (this._connecting) return  // 防止重复连接
    this._connecting = true

    const client = mqtt.connect(MQTT_CONFIG.url, MQTT_CONFIG.options)

    client.then(() => {
      console.log('[MQTT] 连接成功')
      this._connecting = false
      this.globalData.mqttConnected = true
      this.globalData.mqttClient = client
      this.globalData.deviceStatus.status = 'active'

      // 订阅核心主题
      this._subscribeTopics(client)
    }).catch(err => {
      console.error('[MQTT] 连接失败:', err)
      this._connecting = false
      this.globalData.deviceStatus.status = 'offline'
      // 10 秒后重试
      setTimeout(() => this._connectMqtt(), 10000)
    })

    // 注册消息回调
    client.on('message', (topic, payload) => {
      this._handleMessage(topic, payload)
    })

    client.on('close', () => {
      console.log('[MQTT] 连接断开')
      this.globalData.mqttConnected = false
      this.globalData.deviceStatus.status = 'offline'
      // 5 秒后重连
      setTimeout(() => {
        this._connecting = false
        this._connectMqtt()
      }, 5000)
    })

    client.on('error', (err) => {
      console.error('[MQTT] 错误:', err)
    })
  },

  // ── 订阅主题 ─────────────────────────────────────────

  _subscribeTopics(client) {
    const topics = [
      'bookshelf/sensors',   // 环境传感器
      'bookshelf/shelf',     // 书架全量快照
      'bookshelf/event',     // 借/还/错放事件
      'bookshelf/state',     // 状态机状态
      'bookshelf/status',    // 设备在线状态
      'bookshelf/device/status', // 无 UHF 时设备状态
      'bookshelf/error'      // 故障告警
    ]
    client.subscribe(topics, 0)
    console.log('[MQTT] 已订阅:', topics.join(', '))
  },

  // ── 消息处理 ─────────────────────────────────────────

  _handleMessage(topic, payload) {
    try {
      if (typeof payload !== 'string') {
        payload = String(payload || '')
      }
      const trimmed = payload.trim()
      if (!trimmed || (trimmed[0] !== '{' && trimmed[0] !== '[')) {
        console.warn('[MQTT] 忽略非 JSON 消息:', topic, trimmed.slice(0, 100))
        return
      }

      const data = JSON.parse(trimmed)
      console.log('[MQTT] 收到:', topic, JSON.stringify(data).slice(0, 100))

      switch (topic) {
        case 'bookshelf/sensors':
          this.globalData.deviceStatus = {
            ...(this.globalData.deviceStatus || {}),
            status: 'active',
            lastSeen: data.timestamp || new Date().toLocaleString()
          }
          this._updateSensorData(data)
          break
        case 'bookshelf/shelf':
          this.globalData.shelfData = data
          break
        case 'bookshelf/event':
          this.globalData.deviceStatus = {
            ...(this.globalData.deviceStatus || {}),
            status: 'active',
            lastSeen: data.timestamp || new Date().toLocaleString()
          }
          this._addEvent(data)
          break
        case 'bookshelf/state':
          this.globalData.deviceStatus = {
            ...(this.globalData.deviceStatus || {}),
            status: 'active',
            lastSeen: data.timestamp || new Date().toLocaleString()
          }
          this.globalData.deviceState = data
          break
        case 'bookshelf/status':
        case 'bookshelf/device/status':
          const nextStatus = data.online === false
            ? 'offline'
            : data.status === 'online'
              ? 'active'
              : (data.status || 'sleep')
          this.globalData.deviceStatus = {
            status: nextStatus,
            lastSeen: data.timestamp || new Date().toLocaleString()
          }
          this.globalData.deviceState = data
          break
        case 'bookshelf/error':
          console.warn('[MQTT] 设备告警:', data.message)
          this.globalData.lastError = data
          break
      }

      // 通知所有页面数据已更新
      this._notifyPages(topic, data)
    } catch (e) {
      console.error('[MQTT] 解析消息失败:', topic, payload, e)
    }
  },

  _updateSensorData(data) {
    this.globalData.sensorData = {
      temperature: data.temperature != null ? data.temperature.toFixed(1) + '°C' : '--',
      humidity: data.humidity != null ? data.humidity.toFixed(0) + '%' : '--',
      lux: data.lux != null ? Math.round(data.lux) + ' lx' : '--',
      radar: data.radar || null
    }
  },

  _addEvent(event) {
    const events = this.globalData.events
    events.unshift({
      ...event,
      id: Date.now()
    })
    // 只保留最近 50 条
    if (events.length > 50) events.length = 50
  },

  // ── 页面间通信 ───────────────────────────────────────

  _pageCallbacks: [],

  /** 注册页面数据变更回调 */
  onDataChange(callback) {
    this._pageCallbacks.push(callback)
    // 返回取消注册函数
    return () => {
      const idx = this._pageCallbacks.indexOf(callback)
      if (idx >= 0) this._pageCallbacks.splice(idx, 1)
    }
  },

  _notifyPages(topic, data) {
    this._pageCallbacks.forEach(cb => {
      try { cb(topic, data) } catch(e) {}
    })
  },

  // ── 公共方法 ─────────────────────────────────────────

  /** 获取 MQTT 客户端 (需检查 connected 状态) */
  getMqttClient() {
    return this.globalData.mqttClient
  },

  /** 检查是否已连接 */
  isConnected() {
    return this.globalData.mqttConnected
  },

  // ── 云端 API ──────────────────────────────────────────

  /** 检查云端服务状态 + 预加载公共数据 */
  checkCloud() {
    api.getHealth()
      .then(data => {
        this.globalData.cloudStatus = 'online'
        this.globalData.cloudVersion = data.version || ''
        console.log('[Cloud] 服务在线 v' + data.version)
      })
      .catch(() => {
        this.globalData.cloudStatus = 'offline'
        console.warn('[Cloud] 服务不可达')
      })

    // 预加载公共数据（今日统计、热门、环境摘要）
    api.getDailyStats().then(d => {
      this.globalData.dailyStats = d
    }).catch(() => {})

    api.getPopularBooks(10).then(list => {
      this.globalData.popularBooks = list
    }).catch(() => {})

    api.getSensorsSummary().then(s => {
      this.globalData.sensorSummary = s
    }).catch(() => {})
  },

  /** 刷新云端数据（页面 onShow 时调用） */
  refreshCloudData() {
    api.getDailyStats().then(d => {
      this.globalData.dailyStats = d
    }).catch(() => {})
    api.getPopularBooks(10).then(list => {
      this.globalData.popularBooks = list
    }).catch(() => {})
  }
})
