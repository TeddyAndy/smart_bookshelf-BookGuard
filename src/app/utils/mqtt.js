/**
 * 轻量 MQTT 3.1.1 客户端 — 基于 wx.connectSocket
 *
 * 支持: connect / subscribe / publish / ping / 消息回调
 * 用于微信小程序连接 EMQX Broker
 *
 * 用法:
 *   const mqtt = require('../../utils/mqtt')
 *   const client = mqtt.connect('ws://example.com:8083/mqtt', {
 *     clientId: 'mini_program_' + Date.now(),
 *     username: 'mini_program',
 *     password: 'change-me',
 *     keepAlive: 60
 *   })
 *   client.on('message', (topic, payload) => { ... })
 *   client.subscribe('bookshelf/sensors')
 *   client.publish('bookshelf/cmd/scan', JSON.stringify({force: true}))
 */

// ── 协议常量 ──────────────────────────────────────────────
const CMD = {
  CONNECT:    0x10,
  CONNACK:    0x20,
  PUBLISH:    0x30,
  PUBACK:     0x40,
  SUBSCRIBE:  0x82,
  SUBACK:     0x90,
  PINGREQ:    0xc0,
  PINGRESP:   0xd0,
  DISCONNECT: 0xe0,
}

// ── 编码工具 ──────────────────────────────────────────────

/** 编码 MQTT 剩余长度 (可变长度 1-4 字节) */
function encodeLength(len) {
  const bytes = []
  do {
    let byte = len % 128
    len = Math.floor(len / 128)
    if (len > 0) byte |= 0x80
    bytes.push(byte)
  } while (len > 0)
  return new Uint8Array(bytes)
}

/** 解码 MQTT 剩余长度, 返回 [length, bytesConsumed] */
function decodeLength(buffer, offset) {
  let len = 0, multiplier = 1, bytes = 0
  while (true) {
    const byte = buffer[offset + bytes]
    len += (byte & 0x7f) * multiplier
    bytes++
    if ((byte & 0x80) === 0) break
    multiplier *= 128
    if (bytes >= 4) break
  }
  return [len, bytes]
}

/** 写入 UTF-8 字符串 (2 字节长度前缀 + 字符串内容) */
function writeString(buffer, offset, str) {
  const bytes = str instanceof Uint8Array ? str : new TextEncoder().encode(String(str))
  buffer[offset] = (bytes.length >> 8) & 0xff
  buffer[offset + 1] = bytes.length & 0xff
  buffer.set(bytes, offset + 2)
  return offset + 2 + bytes.length
}

/** 读 UTF-8 字符串 */
function readString(buffer, offset) {
  const len = ((buffer[offset] << 8) | buffer[offset + 1]) & 0xffff
  const decoder = new TextDecoder()
  return decoder.decode(buffer.slice(offset + 2, offset + 2 + len))
}

// ── 包构建 ────────────────────────────────────────────────

function buildConnectPacket(opts) {
  const clientId = opts.clientId || 'wx_mini_' + Math.random().toString(36).slice(2, 10)
  const username = opts.username || ''
  const password = opts.password || ''
  const keepAlive = opts.keepAlive || 60
  const cleanSession = opts.cleanSession !== false

  // 计算 payload 长度
  const enc = new TextEncoder()
  const clientIdBytes = enc.encode(clientId)
  const userBytes = enc.encode(username)
  const passBytes = enc.encode(password)

  let payloadLen = 2 + clientIdBytes.length    // clientId
  if (username) payloadLen += 2 + userBytes.length
  if (password) payloadLen += 2 + passBytes.length

  // 可变头: 协议名 "MQTT" (2+4) + 协议级别(1) + 连接标志(1) + keepAlive(2)
  const varHeaderLen = 2 + 4 + 1 + 1 + 2
  const totalLen = varHeaderLen + payloadLen

  const remainBytes = encodeLength(totalLen)
  const buf = new Uint8Array(1 + remainBytes.length + totalLen)
  let pos = 0

  // 固定头
  buf[pos++] = CMD.CONNECT
  buf.set(remainBytes, pos); pos += remainBytes.length

  // 可变头: 协议名
  pos = writeString(buf, pos, 'MQTT')
  // 协议级别
  buf[pos++] = 0x04
  // 连接标志
  let flags = 0x02  // clean session
  if (username) flags |= 0x80
  if (password) flags |= 0x40
  buf[pos++] = flags
  // keepAlive
  buf[pos++] = (keepAlive >> 8) & 0xff
  buf[pos++] = keepAlive & 0xff

  // payload
  pos = writeString(buf, pos, clientId)
  if (username) pos = writeString(buf, pos, username)
  if (password) pos = writeString(buf, pos, password)

  return buf.buffer
}

function buildSubscribePacket(topics, packetId) {
  let payloadLen = 0
  const topicData = topics.map(t => {
    const enc = new TextEncoder()
    const bytes = enc.encode(t.topic || t)
    payloadLen += 2 + bytes.length + 1  // topic + QoS
    return { bytes, qos: t.qos || 0 }
  })

  const varHeaderLen = 2  // packet identifier
  const totalLen = varHeaderLen + payloadLen
  const remainBytes = encodeLength(totalLen)
  const buf = new Uint8Array(1 + remainBytes.length + totalLen)
  let pos = 0

  buf[pos++] = CMD.SUBSCRIBE
  buf.set(remainBytes, pos); pos += remainBytes.length
  buf[pos++] = (packetId >> 8) & 0xff
  buf[pos++] = packetId & 0xff

  for (const td of topicData) {
    pos = writeString(buf, pos, td.bytes)
    buf[pos++] = td.qos
  }

  return buf.buffer
}

function buildPublishPacket(topic, payload, packetId, qos) {
  const enc = new TextEncoder()
  const topicBytes = enc.encode(topic)
  const payloadBytes = typeof payload === 'string' ? enc.encode(payload) : new Uint8Array(payload)

  let varHeaderLen = 2 + topicBytes.length
  if (qos > 0) varHeaderLen += 2  // packetId

  const totalLen = varHeaderLen + payloadBytes.length
  const remainBytes = encodeLength(totalLen)
  const buf = new Uint8Array(1 + remainBytes.length + totalLen)
  let pos = 0

  let cmd = CMD.PUBLISH
  if (qos === 1) cmd |= 0x02
  else if (qos === 2) cmd |= 0x04
  buf[pos++] = cmd
  buf.set(remainBytes, pos); pos += remainBytes.length
  pos = writeString(buf, pos, topic)
  if (qos > 0) {
    buf[pos++] = (packetId >> 8) & 0xff
    buf[pos++] = packetId & 0xff
  }
  buf.set(payloadBytes, pos)

  return buf.buffer
}

// ── 包解析 ────────────────────────────────────────────────

function parsePacket(buffer) {
  if (!buffer || buffer.byteLength < 2) return null

  const bytes = new Uint8Array(buffer)
  const cmd = bytes[0] & 0xf0
  const [remainLen, rlBytes] = decodeLength(bytes, 1)
  const headerLen = 1 + rlBytes

  if (buffer.byteLength < headerLen + remainLen) return null  // 不完整

  const result = { cmd, remainLen, headerLen, totalLen: headerLen + remainLen }

  // CONNACK
  if (cmd === CMD.CONNACK) {
    result.returnCode = bytes[headerLen + 1]
  }

  // SUBACK
  if (cmd === CMD.SUBACK) {
    result.packetId = (bytes[headerLen] << 8) | bytes[headerLen + 1]
  }

  // PUBLISH
  if (cmd === CMD.PUBLISH) {
    const qos = (bytes[0] & 0x06) >> 1
    let pos = headerLen
    const topic = readString(bytes, pos)
    pos += 2 + ((bytes[pos] << 8) | bytes[pos + 1]) & 0xffff

    let payloadStart = pos
    if (qos > 0) {
      result.packetId = (bytes[pos] << 8) | bytes[pos + 1]
      payloadStart += 2
    }

    const decoder = new TextDecoder()
    result.topic = topic
    result.payload = decoder.decode(bytes.slice(payloadStart, headerLen + remainLen))
    result.qos = qos
  }

  // PINGRESP
  if (cmd === CMD.PINGRESP) {
    result.ping = true
  }

  return result
}

// ── MQTT Client ───────────────────────────────────────────

class MqttClient {
  constructor() {
    this._opts = {}
    this._socket = null
    this._connected = false
    this._packetId = 1
    this._callbacks = {}
    this._pingTimer = null
    this._pendingTopics = []   // 缓存在连接前的订阅
    this._buffer = new Uint8Array(0)  // 接收缓冲
  }

  /**
   * 连接到 MQTT Broker
   * @param {string} url - ws://host:port/mqtt
   * @param {object} opts - { clientId, username, password, keepAlive, cleanSession }
   */
  connect(url, opts = {}) {
    this._opts = { ...opts }
    this._opts.clientId = opts.clientId || 'wx_mini_' + Date.now() + '_' + Math.random().toString(36).slice(2, 6)

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('MQTT 连接超时')), 10000)

      this._socket = wx.connectSocket({
        url: url,
        header: { 'Content-Type': 'application/json' },
        protocols: ['mqtt'],
        success: () => {},
        fail: (err) => { clearTimeout(timeout); reject(err) }
      })

      this._socket.onOpen(() => {
        // WebSocket 已连接，发送 MQTT CONNECT
        const packet = buildConnectPacket(this._opts)
        this._socket.send({ data: packet })
      })

      this._socket.onMessage((res) => {
        this._handleData(res.data)
      })

      this._socket.onClose(() => {
        this._connected = false
        this._stopPing()
        if (this._callbacks.close) this._callbacks.close()
      })

      this._socket.onError((err) => {
        clearTimeout(timeout)
        this._connected = false
        this._stopPing()
        if (this._callbacks.error) this._callbacks.error(err)
        reject(err)
      })

      // CONNACK 处理
      const onConnack = (packet) => {
        clearTimeout(timeout)
        if (packet.returnCode === 0) {
          this._connected = true
          this._startPing()
          // 补发缓存的订阅
          this._pendingTopics.forEach(t => this._doSubscribe(t))
          this._pendingTopics = []
          resolve({ clientId: this._opts.clientId })
        } else {
          const errors = ['接受', '协议版本拒绝', 'ClientId 拒绝', '服务器不可用', '用户名密码错误', '未授权']
          reject(new Error('MQTT 连接拒绝: ' + (errors[packet.returnCode] || 'code=' + packet.returnCode)))
        }
      }

      this._callbacks._connack = onConnack
    })
  }

  /**
   * 订阅主题
   * @param {string|string[]} topics - 主题或主题数组
   * @param {number} [qos=0]
   */
  subscribe(topics, qos = 0) {
    const topicList = Array.isArray(topics) ? topics : [{ topic: topics, qos }]
    if (typeof topics === 'string') topicList[0] = { topic: topics, qos }

    if (!this._connected) {
      this._pendingTopics.push(topicList)
      return
    }
    this._doSubscribe(topicList)
  }

  _doSubscribe(topicList) {
    const packetId = this._packetId++
    const packet = buildSubscribePacket(topicList, packetId)
    this._socket.send({ data: packet })

    // 注册 SUBACK 等待
    const topics = topicList.map(t => t.topic || t)
    const cb = this._callbacks._suback
    if (cb) cb(topics, packetId)
  }

  /**
   * 发布消息
   * @param {string} topic - 主题
   * @param {string|object} payload - 消息内容
   * @param {number} [qos=1]
   */
  publish(topic, payload, qos = 1) {
    if (!this._connected) {
      console.warn('[MQTT] 未连接，无法发布')
      return
    }

    const packetId = this._packetId++
    const data = typeof payload === 'object' ? JSON.stringify(payload) : String(payload)
    const packet = buildPublishPacket(topic, data, packetId, qos)
    this._socket.send({ data: packet })
    console.log('[MQTT] 发布:', topic, data.slice(0, 80))
  }

  /** 断开连接 */
  disconnect() {
    this._stopPing()
    if (this._connected) {
      try { this._socket.send({ data: new Uint8Array([CMD.DISCONNECT, 0]).buffer }) } catch(e) {}
    }
    this._connected = false
    setTimeout(() => {
      try { this._socket.close({}) } catch(e) {}
    }, 200)
  }

  // ── 事件监听 ──────────────────────────────────────────

  /**
   * 注册事件回调
   * @param {'message'|'close'|'error'} event
   * @param {function} callback
   */
  on(event, callback) {
    this._callbacks[event] = callback
  }

  // ── 内部方法 ──────────────────────────────────────────

  _handleData(data) {
    // 兼容 ArrayBuffer / Uint8Array / String 三种格式
    let chunk
    if (data instanceof ArrayBuffer) {
      chunk = new Uint8Array(data)
    } else if (data instanceof Uint8Array) {
      chunk = data
    } else if (typeof data === 'string') {
      chunk = new TextEncoder().encode(data)
    } else {
      // 兜底: 微信有时返回带 buffer 属性的对象
      chunk = new Uint8Array(data.buffer || data)
    }
    const merged = new Uint8Array(this._buffer.length + chunk.length)
    merged.set(this._buffer, 0)
    merged.set(chunk, this._buffer.length)
    this._buffer = merged

    // 尝试解析包
    while (true) {
      const packet = parsePacket(this._buffer.buffer)
      if (!packet) break  // 数据不完整

      this._buffer = this._buffer.slice(packet.totalLen)

      if (packet.cmd === CMD.CONNACK && this._callbacks._connack) {
        this._callbacks._connack(packet)
        delete this._callbacks._connack
      }

      if (packet.cmd === CMD.PUBLISH && this._callbacks.message) {
        this._callbacks.message(packet.topic, packet.payload)
      }

      if (packet.cmd === CMD.PINGRESP) {
        // 心跳响应，静默处理
      }
    }
  }

  _startPing() {
    this._stopPing()
    const interval = ((this._opts.keepAlive || 60) * 1000) / 2
    this._pingTimer = setInterval(() => {
      if (this._connected) {
        try { this._socket.send({ data: new Uint8Array([CMD.PINGREQ, 0]).buffer }) } catch(e) {}
      }
    }, Math.min(interval, 30000))
  }

  _stopPing() {
    if (this._pingTimer) {
      clearInterval(this._pingTimer)
      this._pingTimer = null
    }
  }

  /** 是否已连接 */
  get connected() {
    return this._connected
  }
}

// ── 导出 ─────────────────────────────────────────────────

/**
 * 创建并连接 MQTT 客户端
 * @param {string} url
 * @param {object} opts
 * @returns {MqttClient} - 支持 then/catch，可 await 连接就绪
 */
function connect(url, opts) {
  const client = new MqttClient()
  const promise = client.connect(url, opts)

  // 让 client 支持 then/catch，方便 await 连接就绪
  client.then = (onFulfilled, onRejected) => promise.then(onFulfilled, onRejected)
  client.catch = (onRejected) => promise.catch(onRejected)

  promise.catch(err => {
    console.error('[MQTT] 连接失败:', err)
  })

  return client
}

module.exports = { connect, MqttClient }
