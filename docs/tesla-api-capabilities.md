# 特斯拉车辆能力清单（状态读取 / 车辆控制）

> 依据本仓库 `3.49文档/vehicle-command`（Tesla 官方 `teslamotors/vehicle-command` Go SDK）源码整理。
> 对应代码：`pkg/vehicle/*.go`（104 个公开方法）、`pkg/protocol/protobuf/*.proto`（消息定义）。

---

## 0. 前置概念：两条命令通道

| 通道 | 处理方 | 特点 | 典型能力 |
|---|---|---|---|
| **VCSEC**（`DOMAIN_VEHICLE_SECURITY`） | 车辆安全控制器 | 车机休眠时仍可用，蓝牙即可 | 锁车/解锁、远程启动、前后备箱、钥匙白名单、车身状态 |
| **Infotainment**（`DOMAIN_INFOTAINMENT`） | 车机（中控） | 需要车机在线/唤醒，走 carserver 动作 | 空调、充电、媒体、软件升级、哨兵、代客模式、家长控制 |

- 两个域**各自独立密钥对**，需分别握手（`StartSession`）。
- 认证方式由传输决定：BLE 用 **AES-GCM-ECDH**；Fleet API（HTTPS）用 **HMAC-SHA256-ECDH**。
- 密钥必须先在车上白名单注册（`SendAddKeyRequest` + 车机刷 NFC 卡）。

---

## 1. 可以读取的状态信息

SDK 的读取接口只有两个，但覆盖面很广：

```go
BodyControllerState(ctx) (*vcsec.VehicleStatus, error)   // 不需唤醒车机
GetState(ctx, category StateCategory) (*carserver.VehicleData, error) // 按类别取，一次一类
```

### 1.1 车身控制器状态（VCSEC）

`BodyControllerState()` 走 VCSEC，**车机睡着也能查**。包含：

- 四门、前备箱、后备箱开闭状态
- 车窗、天窗开度
- 锁车状态 `locked`
- 是否有人在车内 `is_user_present`
- 中控屏状态（关机/变暗/附件/开机/行驶/充电/锁定/哨兵/宠物/娱乐）
- 远程启动是否激活 `remote_start`
- 代客模式状态、是否需要代客 PIN
- 哨兵模式状态（关闭/空闲/已布防/警戒/告警/静默）+ 是否可用
- 限速模式（是否激活、是否设了 PIN、当前/最大/最小限速）
- Cybertruck 货箱盖状态、开度、是否在运动
- 充电口状态

### 1.2 车辆数据（`GetState`，12 类）

| 类别 | 能读到什么 |
|---|---|
| `StateCategoryCharge`（充电） | 充电状态（未连接/无电/启动中/充电中/完成/已停止/校准中）、电池百分比与可用百分比、续航（rated/est/ideal）、充电限值 SOC、已充入电量与里程、充电电压/电流/功率/相数、充满剩余时间、充电速率、充电口开闭与锁止、充电口灯色、快充类型与品牌、充电线缆类型、预约充电与预约出发（含离峰时段）、当前充电电流请求/上限、Managed Charging 与太阳能充电状态、对外放电（PowerShare/插座）状态与剩余时间、家/公司坐标、单次限值 |
| `StateCategoryClimate`（空调） | 车内/车外温度、主副驾设定温度、风量、是否开启、前/后除霜、9 个座位加热档位、前排座椅通风、方向盘加热与档位、电池加热器、是否预热中、Climate Keeper 模式（Off/On/Dog/Party）、生化防御模式、座舱过热保护（状态/是否制冷/触发温度/未运行原因）、自动座椅气候、可用温度范围 |
| `StateCategoryDrive`（驾驶） | 档位（P/R/N/D/无效/SNA）、速度（整数与浮点）、功率、总里程、导航路线（目的地、剩余分钟/英里、路况延误、到达剩余电量、路线坐标、路线与路况更新时间） |
| `StateCategoryLocation`（定位） | 经纬度、航向、GPS 时间戳、原始 GPS（经纬度/海拔/精度）、国内坐标系（GCJ/WGS）与"纠偏后"坐标、模糊地名 `location_name`、是否靠近 HomeLink 地点 |
| `StateCategoryClosures`（开闭件） | 四门与前后备箱、四窗、天窗状态与开度、锁车、车内是否有人、中控屏状态、远程启动、代客模式、哨兵模式、限速模式、货箱盖 |
| `StateCategoryChargeSchedule`（充电计划） | 全部充电计划（ID、名称、星期掩码、起始/结束时间、是否启用、单次、经纬度）、计划时间窗、充电缓冲、最大计划数 |
| `StateCategoryPreconditioningSchedule`（预热计划） | 全部预热计划（ID、名称、星期、预热时间、单次、启用）、时间窗、最大计划数 |
| `StateCategoryTirePressure`（胎压） | 四轮胎压（bar）、各自最近上报时间、硬告警/软告警标志、前后推荐胎压 |
| `StateCategoryMedia`（媒体） | 正在播放的艺术家/曲名、音量/步进/最大音量、音源类型、播放状态、是否允许远程控制 |
| `StateCategoryMediaDetail`（媒体详情） | 曲目总时长与已播放时长、音源字符串、专辑、电台、A2DP 设备名 |
| `StateCategorySoftwareUpdate`（OTA） | 状态（未知/安装中/已排期/有可用更新/等待 WiFi/下载中）、排期时间、剩余警告时长、预计耗时、下载/安装进度、版本号 |
| `StateCategoryParentalControls`（家长控制） | 是否启用、是否已设 PIN、各项设置（限速开关与当前/最大/最小限速、加速限制、强制安全设置、宵禁起止时间） |

> 注意：**没有**"一次性返回全部车辆数据"的接口，12 类需要各发一次请求；公网场景应改用 Fleet API 的 `vehicle_data`（带缓存、一次聚合）。

---

## 2. 车辆控制命令

### 2.1 门锁与远程启动（VCSEC）

| 方法 | 说明 |
|---|---|
| `Lock(ctx)` | 锁车 |
| `Unlock(ctx)` | 解锁 |
| `RemoteDrive(ctx)` | 远程启动（RKE，等效"无钥匙启动"） |
| `AutoSecureVehicle(ctx)` | 自动上锁（RKE） |
| `Wakeup(ctx)` | 唤醒车辆：BLE 走 RKE 唤醒，Fleet API 走 `wake_up` |

### 2.2 开闭件（VCSEC）

| 方法 | 说明 |
|---|---|
| `ActuateTrunk(ctx)` | 后备箱开/关（按当前状态取反） |
| `OpenTrunk(ctx)` / `CloseTrunk(ctx)` | 后备箱开/关 |
| `OpenFrunk(ctx)` | 前备箱开 |
| `OpenTonneau(ctx)` / `CloseTonneau(ctx)` / `StopTonneau(ctx)` | Cybertruck 货箱盖 开/关/停 |

### 2.3 灯光、喇叭、车窗、充电口

| 方法 | 说明 |
|---|---|
| `FlashLights(ctx)` | 闪灯 |
| `HonkHorn(ctx)` | 鸣笛 |
| `ChangeSunroofState(ctx, sunroofLevel int32)` | 天窗开度控制 |
| `CloseWindows(ctx)` | 关窗 |
| `VentWindows(ctx)` | 开窗通风 |
| `ChargePortOpen(ctx)` / `ChargePortClose(ctx)` | 充电口开/关（车机通道） |
| `OpenChargePort(ctx)` / `CloseChargePort(ctx)` | 同上，**别名**（`charge.go` 与 `actions.go` 重复实现） |

### 2.4 空调与座椅（车机）

| 方法 | 参数要点 |
|---|---|
| `ClimateOn(ctx)` / `ClimateOff(ctx)` | 开/关空调 |
| `ChangeClimateTemp(ctx, driverC, passengerC float32)` | 主驾/副驾目标温度（℃） |
| `SetSeatHeater(ctx, levels map[SeatPosition]Level)` | 一次设置多个座位加热档位 |
| `SetSeatCooler(ctx, level Level, seat SeatPosition)` | 座椅通风档位 |
| `AutoSeatAndClimate(ctx, positions []SeatPosition, enabled bool)` | 自动座椅气候 |
| `SetSteeringWheelHeater(ctx, enabled bool)` | 方向盘加热 |
| `SetPreconditioningMax(ctx, enabled, manualOverride bool)` | 最大预热/预冷 |
| `SetBioweaponDefenseMode(ctx, enabled, manualOverride bool)` | 生化防御模式（需 HEPA） |
| `SetCabinOverheatProtection(ctx, enabled, fanOnly bool)` | 座舱过热保护 |
| `SetCabinOverheatProtectionTemperature(ctx, level Level)` | 过热保护触发温度档 |
| `SetClimateKeeperMode(ctx, mode ClimateKeeperMode, override bool)` | 保持空调：`Off/On/Dog/Camp` |

`Level`：`LevelOff / LevelLow / LevelMed / LevelHigh`
`SeatPosition`：`SeatFrontLeft/Right`、`SeatSecondRowLeft(+Back)/Center/Right(+Back)`、`SeatThirdRowLeft/Right`

### 2.5 充电

| 方法 | 说明 |
|---|---|
| `ChargeStart(ctx)` / `ChargeStop(ctx)` | 开始/停止充电 |
| `ChangeChargeLimit(ctx, percent int32)` | 设置充电限值 SOC |
| `SetChargingAmps(ctx, amps int32)` | 设置充电电流 |
| `ChargeMaxRange(ctx)` / `ChargeStandardRange(ctx)` | 切到"最大续航"/"标准续航"限值 |
| `ScheduleCharging(ctx, enabled bool, timeAfterMidnight time.Duration)` | 预约充电（当天零点后多久开始） |
| `ScheduleDeparture(ctx, departAt, offPeakEnd time.Duration, precond, offpeak ChargingPolicy)` | 预约出发 + 离峰充电 |
| `ClearScheduledDeparture(ctx)` | 清除预约出发 |
| `AddChargeSchedule(ctx, *ChargeSchedule)` | 新增充电计划（ID/名称/星期/起止时间/单次/经纬度） |
| `RemoveChargeSchedule(ctx, id uint64)` | 删除单个充电计划 |
| `BatchRemoveChargeSchedules(ctx, home, work, other bool)` | 批量删除充电计划 |
| `AddPreconditionSchedule(ctx, *PreconditionSchedule)` | 新增预热计划 |
| `RemovePreconditionSchedule(ctx, id uint64)` | 删除单个预热计划 |
| `BatchRemovePreconditionSchedules(ctx, home, work, other bool)` | 批量删除预热计划 |

### 2.6 电源管理

| 方法 | 说明 |
|---|---|
| `SetLowPowerMode(ctx, enable bool)` | 低功耗模式（降低静态耗电） |
| `SetKeepAccessoryPowerMode(ctx, enable bool)` | 保持附件电源（点烟器/USB 常电） |

### 2.7 媒体与车机（车机）

| 方法 | 说明 |
|---|---|
| `Ping(ctx)` | 探活 |
| `VolumeUp(ctx)` / `VolumeDown(ctx)` / `SetVolume(ctx, volume float32)` | 音量（0.0–10.0） |
| `MediaNextTrack(ctx)` / `MediaPreviousTrack(ctx)` | 上/下一曲 |
| `MediaNextFavorite(ctx)` / `MediaPreviousFavorite(ctx)` | 上/下一个收藏 |
| `ToggleMediaPlayback(ctx)` | 播放/暂停切换 |
| `ScheduleSoftwareUpdate(ctx, delay time.Duration)` | 预约 OTA |
| `CancelSoftwareUpdate(ctx)` | 取消 OTA |
| `GetNearbyCharging(ctx)` | 查询附近充电站 |
| `SetVehicleName(ctx, name string)` | 修改车辆名称 |

### 2.8 安全、钥匙与家长控制

| 方法 | 说明 |
|---|---|
| `SetSentryMode(ctx, state bool)` | 哨兵模式开关 |
| `SetValetMode(ctx, on bool, pin string)` / `EnableValetMode(ctx, pin)` / `DisableValetMode(ctx)` | 代客模式 |
| `ResetValetPin(ctx)` | 重置代客 PIN |
| `ResetPIN(ctx)` | 重置（通用）PIN |
| `SetPINToDrive(ctx, enabled bool, pin string)` / `ClearPINToDrive(ctx)` | 行车 PIN |
| `ActivateSpeedLimit(ctx, pin string)` / `DeactivateSpeedLimit(ctx, pin string)` | 限速模式开关 |
| `SpeedLimitSetLimitMPH(ctx, mph float64)` | 设置限速值 |
| `ClearSpeedLimitPIN(ctx, pin string)` / `ClearSpeedLimitPINAdminAction(ctx)` | 清除限速 PIN |
| `TriggerHomelink(ctx, lat, lon float32)` | 触发 HomeLink（车库门/门禁） |
| `SetGuestMode(ctx, enabled bool)` / `EraseGuestData(ctx)` | 访客模式 / 清除访客数据 |
| `SendAddKeyRequest(ctx, pub, isOwner bool, formFactor)` | 发配对请求（**用户到车机刷 NFC 卡确认**） |
| `SendAddKeyRequestWithRole(ctx, pub, role, formFactor)` | 带角色的配对请求 |
| `AddKey(ctx, pub, isOwner, formFactor)` / `AddKeyWithRole(ctx, pub, role, formFactor)` | 直接把公钥加入白名单（需已有 owner 权限） |
| `RemoveKey(ctx, pub)` | 移除钥匙 |
| `KeySummary(ctx)` | 白名单概览（条目数、slot 掩码） |
| `KeyInfoBySlot(ctx, slot uint32)` | 单个钥匙槽详情 |
| `ParentalControlsActivate(ctx, pin string)` / `ParentalControlsDeactivate(ctx, pin string)` | 家长控制开关 |
| `ParentalControlsEnableSetting(ctx, setting, enable bool)` | 单项设置开关 |
| `ParentalControlsSetSpeedLimit(ctx, limitMPH float64)` | 家长控制限速 |
| `ParentalControlsClearPIN(ctx)` | 清除家长控制 PIN |

`role`：`owner / driver / fm（车队管理员）/ vehicle_monitor / charging_manager`
`formFactor`：`nfc_card / ios_device / android_device / cloud_key`
`ParentalControlsSetting`：`SpeedLimit / Acceleration / SafetyFeatures / Curfew / BrowserBlocked / TheaterBlocked / ArcadeBlocked`

### 2.9 通用逃生口：任意消息

SDK 未封装的动作可以自己拼 protobuf 发出去：

```go
Send(ctx, domain universal.Domain, payload []byte, auth connector.AuthMethod) ([]byte, error)
SendMessage(ctx, message *universal.RoutableMessage) (protocol.Receiver, error)
```

`car_server.proto` 的 `Action_VehicleAction` 里还有大量动作可复用上述通道（例如 `HvacAutoAction`、`VehicleControlWindowAction`、`ScheduledDepartureAction`、`EraseUserDataAction`、`DrivingSpeedLimitAction` 等）。

---

## 3. 明确不支持 / 需改走 REST

| 能力 | 现状 |
|---|---|
| 哨兵鸣笛（Boombox，`remote_boombox`） | `ErrCommandNotImplemented`，SDK 未实现 |
| Managed Charging（`set_managed_charge_current_request` 等 3 条） | `ErrCommandUseRESTAPI`，只能走 Fleet API REST |
| 车辆列表 / 账号信息 / 位置历史 | 属于 Fleet API，不在本 SDK 的 vehicle 包 |
| 数据推送 / 订阅（Fleet Telemetry 数据流） | 无；只能轮询 `GetState` |
| 一次性聚合车辆数据 | 无；需逐类请求或用 Fleet API `vehicle_data` |

---

## 4. 对 ESP32 BLE 仪表的实践建议

1. **开机流程**：`BodyControllerState()` 判断闭件与车机是否休眠 → 需要数据时再 `Wakeup()` + `StartSession(Infotainment)`。
2. **仪表盘数据源**：轮询 `GetState(StateCategoryDrive)`（档位/车速/功率/里程/导航）+ `GetState(StateCategoryCharge)`（电量/充电），与你现有 `drive_state` 实现一致。
3. **低功耗**：车机休眠时只读 VCSEC，不唤醒 infotainment。
4. **配对**：`SendAddKeyRequest(pub, true, KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY)` → 中控点确认 → 刷 NFC 卡。
5. **注意单位**：速度/续航/里程在 proto 里是 **英里**（`speed`/`battery_range`/`odometer_in_hundredths_of_a_mile`），温度是 **摄氏度**，胎压是 **bar**。
