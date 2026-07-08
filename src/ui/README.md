# SmartBookshelf 屏幕 UI 与 FSM 联动说明

这是当前 7 寸 MIPI 屏幕的重写版 LVGL UI，以及它和 `fsm_main` 的联动协议说明。旧版 UI 已整体保存在 `archive/`，只作为参考和保留历史。

当前设计原则：FSM 负责硬件和状态判断，屏幕只负责展示、读本地数据库、向 FSM 写少量命令。

## 运行职责

屏幕进程只做展示和简单命令转发：

- 读取 `/tmp/fsm_state` 获取 FSM 当前状态。
- 读取 `/root/bookshelf.db` 获取书籍、书架实时状态和传感器记录。
- 写入 `/tmp/sb_cmd` 向 FSM 发送查找、取消查找、加书和注册命令。
- 不直接打开 UHF、灯带、雷达、MQTT、I2C 等硬件设备。

这样可以避免屏幕进程和 `fsm_main` 抢硬件资源。

## 当前代码结构

- `main.c`：LVGL 页面布局、书架渲染、查找页、加书页、趋势页、网络页、详情弹窗和页面切换。
- `ui_model.c/.h`：数据库读取、FSM 状态解析、命令写入、搜索匹配。
- `CMakeLists.txt`：屏幕程序构建配置。
- `archive/`：旧 UI 代码、原窗口初始化代码和字体文件；新 UI 仍复用其中的 `window.c` 和 `font_songti_24.c`。

FSM 和数据库侧配套改动：

- `src/fsm/fsm.c/.h`：扩展 `/tmp/fsm_state` 字段，处理 `FIND/ADD/REGISTER/NET_*` 命令，写入查找、加书和网络状态。
- `src/uhf/uhf_scanner.c/.h`：记录实际主读层，用于错放判断；只有加书模式才检测未知 EPC。
- `src/db/db.c/.h`：新增 `db_sensor_range()`，用于近期状态页按时间窗口聚合传感器数据。

## 字体

屏幕使用预生成的 24px 宋体位图字体 `archive/font_songti_24.c`，不依赖运行时 FreeType。

当前字体包含：

- ASCII、数字、常用符号。
- 当前 UI、FSM 状态文件、README 中出现的全部中文。
- 书架业务常用字，例如登记、扫描、保存、取消、环境、建议、课程、书名、错放、取走、传感器等。

如果后续新增大量新文案或特殊书名，仍需要重新生成位图字体。

## 页面

- 书架页：顶部状态、统计条、上下两层书架、底部传感器和导航。
- 查找页：输入框、软键盘、可滚动书目列表，支持中文、EPC、拼音和拼音首字母检索。
- 查找中：显示目标书名、层号和位置，提示看黄色灯光；FSM 检测到目标书被取走后，屏幕会显示“已取出”。
- 加书页：只有进入加书模式后，FSM 才读取未登记 EPC；读到后填写书名和位置保存。
- 近期状态：从本地 `sensor_log` 读取记录，支持实时、1小时、1天、1周范围切换，画温度、湿度、光照趋势，并给出保存环境建议。
- 网络页：显示 wlan0、WiFi、IP、MQTT 状态，支持扫描附近 WiFi、输入密码连接和断开。

底部导航目前为：

- `书架`
- `查找`
- `加书`
- `近期`
- `网络`

传感器由 FSM 读取并写入数据库，目前写入间隔为 5 秒；屏幕只读数据库，不直接读 I2C。

趋势图显示规则：

- 红色：温度，单位 C。
- 蓝色：湿度，单位 %。
- 黄色：光照，按 `lux/10` 缩放到同一张 0-100 标尺上。
- 实时：最近约 5 分钟原始点。
- 1小时：每 30 秒聚合。
- 1天：每 10 分钟聚合。
- 1周：每 1 小时聚合。
- X 轴显示起点、中点、终点时间；实时模式按板端当前时间滚动，1小时/1天/1周按数据库记录时间显示。
- 如果未联网，板端时间可能不准确，但实时 X 轴仍会随本地时间变化。

## 查找闭环

1. 屏幕在查找页选中书籍，写入 `FIND <epc>`。
2. FSM 进入 `FIND`，点亮目标书位置的黄色灯光。
3. FSM 继续盘点；如果目标书被取走，写入 `find_status=2`。
4. 屏幕看到 `find_status=2` 后显示“已取出”。
5. 用户可以点击取消，屏幕写入 `FIND_CANCEL`，FSM 清除查找状态。

`find_status` 含义：

- `0`：空闲
- `1`：查找中
- `2`：目标已取出
- `3`：查找超时
- `4`：已取消

## 加书闭环

普通状态下 FSM 不处理未知 EPC，因为当前硬件环境下串读和杂 EPC 是必然存在的。

加书流程：

1. 屏幕进入加书页并写入 `ADD_START`。
2. FSM 设置 `add_mode=1`，此时才让 scanner 输出未知 EPC。
3. 读到第一个未知 EPC 后，FSM 写入 `register_pending=1`、`pending_epc`、`pending_layer`，并暂停未知 EPC 检测。
4. 屏幕显示待登记 EPC，用户填写书名、起点、终点。
5. 屏幕写入 `REGISTER <layer> <start_cm> <end_cm> <title>`。
6. FSM 将待登记 EPC 写入 `books` 和 `shelf_state`，然后重新加载 scanner 书目。
7. 用户可随时写入 `ADD_CANCEL` 退出加书模式。

## 错放判断

scanner 会记录每本书最近主读层：

- 如果实际主读层等于 `books.expected_layer`，UI 显示正常在架。
- 如果实际主读层不等于 `books.expected_layer`，FSM 将实际层写入 `shelf_state.layer`，UI 在预期层位置显示粉红色错放。

注意：上下层串读仍会影响实际主读层判断。装上屏蔽布后，如果仍有误判，需要继续调整 scanner 的主读层阈值，不建议只靠 UI 修饰。

## FSM 通信协议

屏幕读取 `/tmp/fsm_state`，当前使用字段：

- `state`：`BOOT/ACTIVE/SLEEP/FIND/FAULT`
- `find_status`：`0` 空闲，`1` 查找中，`2` 已取出，`3` 超时，`4` 已取消
- `find_target_epc` / `find_target_title`：当前查找目标
- `add_mode`：是否处于加书模式
- `register_pending`：是否读到待登记 EPC
- `pending_epc` / `pending_layer`：待登记标签和读到的层
- `wifi_hw` / `wifi_connected`：是否检测到无线网卡、是否已连接
- `wifi_ssid` / `wifi_ip` / `wifi_signal` / `wifi_error`：网络页状态
- `mqtt_connected`：MQTT 是否在线
- `radar_seen` / `radar_sleep_reliable` / `radar_last_person`：雷达可信度诊断字段，主要用于排查误休眠

屏幕写入 `/tmp/sb_cmd`，当前命令：

- `FIND <epc>`：查找指定书
- `FIND_CANCEL`：取消查找并清除查找反馈
- `FORCE_STATE <BOOT|ACTIVE|SLEEP|FIND|FAULT>`：强制切换 FSM 状态，供调试用
- `FORCE_WAKE`：强制恢复到 `ACTIVE`
- `ADD_START`：进入加书模式，此时 FSM 才检测未知 EPC
- `ADD_CANCEL`：退出加书模式
- `REGISTER <layer> <start_cm> <end_cm> <title>`：把待登记 EPC 保存为新书
- `NET_SCAN`：扫描附近 WiFi，结果由 FSM 写入 `/tmp/wifi_scan`
- `NET_CONNECT <ssid>|<password>`：连接并保存 WiFi
- `NET_DISCONNECT`：断开 WiFi

## 休眠关屏

UI 每秒读取 `/tmp/fsm_state`。当 `state=SLEEP` 时：

- 写 `/sys/class/backlight/backlight/brightness=0` 并尝试写 `bl_power=4`，关闭背光省电。
- 屏幕上同时盖一层黑色遮罩，作为背光节点不可用时的兜底。
- 状态恢复为 `ACTIVE/FIND/BOOT/FAULT` 后，先写 `bl_power=0`，再恢复休眠前的背光亮度。

屏幕关背光只由 UI 执行，FSM 不直接操作显示设备。

## 颜色规则

- 绿色：已知书籍，且在预期层。
- 粉红色：已知书籍，但层号不符合预期；显示在预期层位置。
- 灰色：已知书籍，但当前不在架。
- 紫粉色：保留给未来未知书展示；当前普通模式不会显示未登记 EPC。

## 构建部署

FSM：

```bash
cd src/uhf && make uhf_scanner.o
cd ../fsm && make -B fsm_main
scp src/fsm/fsm_main root@192.168.0.232:/root/fsm_main
```

屏幕：

```bash
cmake -S src/ui -B /tmp/sb-ui-arm-check \
  -DCMAKE_TOOLCHAIN_FILE=/home/elf/3506-toolchain/host/share/buildroot/toolchainfile.cmake
cmake --build /tmp/sb-ui-arm-check -j4
scp /tmp/sb-ui-arm-check/SmartBookshelf root@192.168.0.232:/root/SmartBookshelf
```

板端启动顺序保持不变：

```bash
/root/fsm_main -v --no-screen
/root/SmartBookshelf
```

安全停止顺序：

```bash
killall SmartBookshelf 2>/dev/null || true
sh /root/stop_fsm.sh
```

不要用 `kill -9` 直接杀 FSM，除非正常停止失败；UHF 模块需要 STOP 帧，否则 CH341/模块容易卡住。

## 当前已知注意事项

- 传感器曲线依赖 FSM 写入 `sensor_log`，当前间隔为 5 秒。
- 屏幕不直接读 I2C，不直接访问 UHF/LED/雷达。
- 位图字体已经覆盖当前 UI 和常用业务字，但特殊生僻书名仍可能需要重新生成字体。
- 加书模式只处理一个待登记 EPC；保存或取消后需要重新开始下一本。
- 错放判断目前基于两层 UHF 的主读层，屏蔽结构会直接影响准确性。
