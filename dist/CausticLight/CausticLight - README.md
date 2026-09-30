# CausticLight — 물가 벽과 천장의 물빛

물이 등불 빛을 튕겨서 근처 벽, 다리 밑, 천장, 물 밑바닥에 일렁이는 빛무늬(물그림자)를 그리는
Skyrim SE SKSE 모드입니다. Community Shaders 파일은 건드리지 않고, 플러그인이 직접 한 번 더 칠합니다.

## 설치

- MO2 에 모드 폴더째 설치하고 `CausticLight.esp` 를 켭니다 (ESL 표시, 마스터 없음).
- 필요: SKSE, Address Library. Skyrim SE / AE (VR 은 지원하지 않음).
- 선택: Community Shaders(없어도 동작), Surface Tides(있으면 실제 물 표면의 출렁임으로 무늬를 그림).

## 무엇이 자동인가

- **물 찾기**: 실내에서 캐릭터 주변 약 25 m 사방을 아래로 훑어 물이 있는 곳을 스스로 찾습니다.
  거친 것부터 촘촘하게 훑어 1초 안에 끝나고, 한 화면에 광선 시간을 2 ms 넘게 쓰지 않습니다.
- **광원**: 물 근처에서 가장 센 광원과 그다음 두 개를 각자의 색과 세기로 반사합니다.
  마법등·횃불처럼 나중에 생긴 빛도 따라갑니다.
- **그늘**: 반사하는 광원 3개가 각자 닿는 물을 따로 계산합니다. 그늘진 물은 그 빛을 튕기지 않아서,
  옆방 등불이 벽을 뚫고 튕기지 않습니다. 물에서 벽까지 가는 길을 다리나 기둥이 막으면 그 벽에도 닿지 않습니다(화면 깊이로 확인).
- **밝기**: 튕긴 빛 ÷ 그 면이 원래 받는 빛. 다리 밑처럼 등불을 등진 면은 진하게, 등불을 정면으로
  받는 벽은 은은하게. 방의 기본 밝기는 셀 주변광, 방향별 주변광(위·아래·옆 따로), 다른 광원으로 계산합니다.
  Community Shaders 의 선형 조명(Linear Lighting)이 켜져 있으면 그 설정을 읽어 같은 방식으로 셉니다.
- **물결**: 날씨·바람·물속 NPC 로 물결 세기가 바뀌고, 튕긴 빛이 그만큼 퍼지며 무늬도 잔잔/보통/거침
  세 종류가 섞입니다. 헤엄치는 사람 주변에는 동그란 물결 고리가 생깁니다.
- **물 밑바닥**: 물 표면에서 꺾여 들어간 빛이 바닥에 줄무늬로 모입니다(깊을수록 흐려짐).
  물이 얼마나 탁한지는 그 물 종류의 물속 안개 거리에서 가져옵니다.
- **야외**: 야외 물도 찾습니다(끄려면 zones.json `"autoZonesOutdoors": false`). 낮에는 햇빛,
  밤에는 달빛을 반사하고, 해·달 쪽으로 광선을 쏴 절벽·나무·다리 그늘을 뺍니다.
  비·눈이 오면 구름이 햇빛을 흩어 물빛 무늬가 생기지 않습니다(흐림은 약하게).

- **실제 물결 (Surface Tides)**: Surface Tides 모드가 있으면 그 물 표면의 출렁임을 빌려 와서, 걷거나 헤엄쳐 생긴 물결까지
  벽에 비칩니다. 없으면 미리 만든 무늬(벽까지 거리에 맞춘 거울 반사 무늬)를 씁니다.
- **거리**: 물빛은 게임의 그림자 거리만큼 보이고, 그림자처럼 끝에서 서서히 사라집니다.

## 광원

- 게임 광원 목록을 직접 읽습니다: 놓인 등불, 들고 있는 횃불, 손의 주문 빛, 마법등, Light Placer 빛, 해와 달.
- 물에 가장 많은 빛을 주는 셋을 반사합니다. 나머지는 "원래 있던 빛"으로 셉니다.
- **Light Placer**: 제자리에 붙은 빛은 놓인 등불처럼 그늘 계산을 받습니다. Community Shaders 의 역제곱 빛(InverseSquare)은 커쉐와 같은 공식으로 감쇠합니다.
- **파티클 라이트**(커쉐·ENB 의 불꽃 광채 빛)는 렌더러 안에서만 만들어져 읽을 수 없습니다. 반사에도 원래 빛에도 들어가지 않습니다.

## Community Shaders / ENB

- 시작할 때 렌더러를 감지해 기록에 한 줄 남깁니다(`renderer light:`).
- **Community Shaders**: 선형 조명·역제곱 빛 설정을 읽어 같은 단위로 셉니다.
- **ENB**: enbseries.ini 의 [ENVIRONMENT] 빛 배율(점광원·직사광·주변광)을 시간대에 맞춰 읽습니다(ENB SDK). ENB 에서는 Surface Tides 연동이 기본으로 꺼져 있습니다(`tidesWatchUnderEnb`).
- 설정 파일의 `renderer` 로 강제로 고를 수 있습니다: auto / cs / enb / vanilla.

### ENB 사용자 확인 부탁 (ENB testers)

1. 기록 `CausticLight.log` 의 `renderer light: ENB ...` 줄과 `ENB light multipliers ...` 줄을 알려 주세요. / Please post the `renderer light: ENB` and `ENB light multipliers` log lines.
2. 실내 물가(등불 옆)에서 벽·천장에 물빛이 보이는지. / Caustics on walls near lit interior water?
3. 물 표면이 깜빡이거나 화면이 깨지지 않는지. / No flicker or corruption on the water or screen?
4. ENB 편집 창(Shift+Enter)을 열고 닫아도 튕기지 않는지. / No crash opening / closing the ENB editor?
5. Surface Tides 를 쓰면(ENB 물 끔) 위 항목을 한 번 더. / If you use Surface Tides with ENB water off, repeat once.

## 콘솔 명령 (단축키 없음)

| 명령 | 뜻 |
|---|---|
| `set CL_Enabled to 0` / `1` | 끄기 / 켜기 |
| `set CL_Strength to N` | 세기 0~50, 이번 게임 동안만 (늘 쓰는 값은 아래 `CausticLight.ini` 의 `fStrength`) |
| `set CL_DebugTint to 1` | 확인용: 구역이 닿는 범위를 평평하게 칠함 |
| `set CL_DebugTint to 2` | 확인용: 튕긴 빛이 닿는 곳 (파랑, 기하학적 몫. 반사율과 무늬는 빠짐) |
| `set CL_DebugTint to 3` | 확인용: 등불이 직접 비추는 양 (주황) |
| `set CL_DebugTint to 0` | 확인용 화면 끄기 |
| `set CL_Corner to 1` | 수동 구역: 조준점에 모서리 (두 번 = 구역 하나, zones.json 에 저장) |
| `set CL_Remove to 1` | 조준점의 수동 구역 지우기 |

값은 세이브에 저장됩니다. 확인용 화면을 켠 채 저장하지 마세요.

## 사용자 설정 (CausticLight.ini)

`SKSE\Plugins\CausticLight.ini` — 게임을 켠 채로 고쳐 저장해도 1~2초 안에 반영됩니다.

| 항목 | 기본 | 뜻 |
|---|---|---|
| `fStrength` | 6.4 | 반사광 세기. 2.14 = 물리 계산 그대로(Cycles 기준 그림), 6.4 = 게임에서 잘 보이게 정한 값, 0~50 |
| `fRippleContrast` | 1.0 | 물무늬 진하기. 0 = 고른 빛, 1 = 물리 그대로, 최대 3 |
| `fFloorStrength` | 1.0 | 물 밑바닥 무늬 세기, 0~5 |
| `bFollowShadowDistance` | 1 | 1 = 물빛 거리를 게임의 그림자 거리(fShadowDistance)에 맞춤 |
| `fMaxDistance` | 3000 | 그림자 거리를 따르지 않을 때 물빛이 사라지는 거리 |
| `bUseSurfaceTides` | 1 | Surface Tides 가 있으면 실제 물결로 무늬를 그림 |

`fStrength` 는 게임을 불러올 때마다 세이브의 값보다 우선합니다. INI 가 없으면 세이브·ESP 값을 씁니다.

## 설정 파일 (고급)

`SKSE\Plugins\CausticLight\zones.json` — 설명이 파일 안에 있습니다. `debug: true` 면 매초 구역 상태,
광원 목록, 그리기 GPU 시간, 물 찾기 결과를 `CausticLight.log` 에 남깁니다.

## 비용

게임 안 측정(RTX 3070 노트북, 화면 전체가 물 구역): 보통 0.14~0.2 ms/화면, 가장 높을 때 약 0.5 ms.

## 한계

- 물에서 벽까지 가는 길의 가림은 화면에 보이는 것만 압니다(화면 밖의 가림은 모름).
- ENB 의 시간대별 빛 배율 곡선은 공개되지 않아 직선으로 섞습니다(가정). ENB 환경은 ENB 사용자 확인 전까지 시험 단계입니다.
- 폭 약 1.6 m 보다 작은 웅덩이는 물 찾기에서 놓칠 수 있습니다.

## 소스와 라이선스

CausticLight 는 GPL-3.0 오픈 소스입니다: https://github.com/Iwillbewarlock/CausticLight

## 크레딧

- **Community Shaders** 개발진: CausticLight 가 거는 엔진 호출 지점 두 곳은 Community Shaders 의 참조에서 왔습니다.
- **NearMidnightNow** (Dynamic Terrain Deformation): 그 호출 지점을 DTD 의 훅에서 배웠습니다.
- **Surface Tides** (넥서스 191955) 와 제작자: 그 물결 자료를 같은 형식으로 읽어 옵니다.
- **ScottieFox** (caustic-volume, MIT): 기본 물빛 무늬 생성 방식이 여기서 출발했습니다. 라이선스 파일을 함께 넣었습니다.
- **CommonLibSSE-NG** (MIT).
