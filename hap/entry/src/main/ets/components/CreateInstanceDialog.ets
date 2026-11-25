import { CreateInstanceOptions } from '../model/InstanceModel';

@CustomDialog
export struct CreateInstanceDialog {
  controller: CustomDialogController;
  onConfirm: (opts: CreateInstanceOptions) => void = (): void => {};

  @State private deviceType: string = 'phone';
  @State private count: number = 1;
  @State private scenario: string = 'normal';
  @State private gpuMode: string = 'vulkan';
  @State private refreshRate: number = 120;
  @State private runMode: string = 'emulator';
  @State private compactDisk: boolean = false;
  @State private namePrefix: string = '';

  @Builder
  private typeCard(key: string, title: string, desc: string, iconRes: Resource) {
    Column({ space: 6 }) {
      Row({ space: 8 }) {
        Stack({ alignContent: Alignment.Center }) {
          Column()
            .width(34)
            .height(34)
            .borderRadius(10)
            .backgroundColor(
              this.deviceType === key
                ? $r('sys.color.comp_emphasize_secondary')
                : $r('sys.color.comp_background_tertiary')
            )
          SymbolGlyph(iconRes)
            .fontSize(18)
            .fontColor([
              this.deviceType === key
                ? $r('sys.color.icon_emphasize')
                : $r('sys.color.icon_secondary')
            ])
        }
        Column({ space: 2 }) {
          Text(title)
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor(
              this.deviceType === key
                ? $r('sys.color.font_emphasize')
                : $r('sys.color.font_primary')
            )
          Text(desc)
            .fontSize(11)
            .fontColor($r('sys.color.font_secondary'))
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
    }
    .layoutWeight(1)
    .padding(12)
    .borderRadius(14)
    .backgroundColor(
      this.deviceType === key
        ? $r('sys.color.comp_emphasize_tertiary')
        : $r('sys.color.background_secondary')
    )
    .border({
      width: this.deviceType === key ? 1.5 : 1,
      color: this.deviceType === key ? $r('sys.color.brand') : Color.Transparent
    })
    .onClick(() => {
      this.deviceType = key;
    })
  }

  @Builder
  private scenarioCard(key: string, title: string, spec: string, desc: string) {
    Column({ space: 4 }) {
      Row() {
        Text(title)
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor(
            this.scenario === key
              ? $r('sys.color.font_emphasize')
              : $r('sys.color.font_primary')
          )
        Blank()
        if (this.scenario === key) {
          SymbolGlyph($r('sys.symbol.checkmark'))
            .fontSize(13)
            .fontColor([$r('sys.color.icon_emphasize')])
        }
      }
      .width('100%')

      Text(spec)
        .fontSize(11)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_secondary'))
      Text(desc)
        .fontSize(11)
        .fontColor($r('sys.color.font_tertiary'))
        .maxLines(2)
    }
    .layoutWeight(1)
    .padding(12)
    .alignItems(HorizontalAlign.Start)
    .borderRadius(14)
    .backgroundColor(
      this.scenario === key
        ? $r('sys.color.comp_emphasize_tertiary')
        : $r('sys.color.background_secondary')
    )
    .border({
      width: this.scenario === key ? 1.5 : 1,
      color: this.scenario === key ? $r('sys.color.brand') : Color.Transparent
    })
    .onClick(() => {
      this.scenario = key;
    })
  }

  @Builder
  private segmentChip(label: string, active: boolean, onTap: () => void) {
    Button({ type: ButtonType.Normal }) {
      Text(label)
        .fontSize(12)
        .fontWeight(active ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(active ? $r('sys.color.font_on_primary') : $r('sys.color.font_primary'))
    }
    .height(30)
    .padding({ left: 14, right: 14 })
    .borderRadius(15)
    .backgroundColor(active ? $r('sys.color.brand') : $r('sys.color.comp_background_tertiary'))
    .onClick(onTap)
  }

  build() {
    Column({ space: 0 }) {
      // Dialog Header
      Row() {
        Row({ space: 8 }) {
          SymbolGlyph($r('sys.symbol.plus_circle'))
            .fontSize(20)
            .fontColor([$r('sys.color.icon_emphasize')])
          Text('新建 Android 设备')
            .fontSize(18)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_primary'))
        }
        Blank()
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.xmark'))
            .fontSize(14)
            .fontColor([$r('sys.color.icon_secondary')])
        }
        .width(32)
        .height(32)
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .onClick(() => {
          this.controller.close();
        })
      }
      .width('100%')
      .padding({ left: 24, right: 20, top: 20, bottom: 14 })

      Divider().color($r('sys.color.comp_divider')).opacity(0.4)

      Scroll() {
        Column({ space: 18 }) {
          // 1. Device Type
          Column({ space: 8 }) {
            Text('设备形态')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))
            Row({ space: 12 }) {
              this.typeCard('phone', '手机', '720×1280 竖屏 · 320 DPI', $r('sys.symbol.phone_1'))
              this.typeCard('tablet', '平板', '1600×900 横屏 · 280 DPI', $r('sys.symbol.pad_fill'))
            }
            .width('100%')
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)

          // 2. Android Version + Count + Name
          Row({ space: 16 }) {
            Column({ space: 6 }) {
              Text('系统镜像')
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_secondary'))
              Row({ space: 8 }) {
                Text('Android 14')
                  .fontSize(13)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_emphasize'))
                Text('API 34 · arm64')
                  .fontSize(11)
                  .fontColor($r('sys.color.font_secondary'))
              }
              .height(36)
              .padding({ left: 12, right: 12 })
              .borderRadius(10)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
              .border({ width: 1, color: $r('sys.color.brand') })
            }
            .alignItems(HorizontalAlign.Start)

            Column({ space: 6 }) {
              Text('创建数量')
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_secondary'))
              Row({ space: 8 }) {
                Button({ type: ButtonType.Normal }) {
                  Text('−')
                    .fontSize(16)
                    .fontColor($r('sys.color.font_primary'))
                }
                .width(32)
                .height(32)
                .borderRadius(8)
                .backgroundColor($r('sys.color.comp_background_tertiary'))
                .enabled(this.count > 1)
                .onClick(() => {
                  if (this.count > 1) this.count -= 1;
                })

                Text(`${this.count}`)
                  .fontSize(14)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_primary'))
                  .width(28)
                  .textAlign(TextAlign.Center)

                Button({ type: ButtonType.Normal }) {
                  Text('+')
                    .fontSize(16)
                    .fontColor($r('sys.color.font_primary'))
                }
                .width(32)
                .height(32)
                .borderRadius(8)
                .backgroundColor($r('sys.color.comp_background_tertiary'))
                .enabled(this.count < 5)
                .onClick(() => {
                  if (this.count < 5) this.count += 1;
                })
              }
              .height(36)
              .alignItems(VerticalAlign.Center)
            }
            .alignItems(HorizontalAlign.Start)

            Column({ space: 6 }) {
              Text('设备名称（可选）')
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_secondary'))
              TextInput({ placeholder: '例如：日常工作 / 游戏测试', text: this.namePrefix })
                .height(36)
                .fontSize(13)
                .fontColor($r('sys.color.font_primary'))
                .placeholderColor($r('sys.color.font_tertiary'))
                .backgroundColor($r('sys.color.background_secondary'))
                .borderRadius(10)
                .maxLength(32)
                .onChange((val: string) => {
                  this.namePrefix = val;
                })
            }
            .layoutWeight(1)
            .alignItems(HorizontalAlign.Start)
          }
          .width('100%')

          // 3. Scenario Presets (MuMu style)
          Column({ space: 8 }) {
            Text('性能场景')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))
            Row({ space: 10 }) {
              this.scenarioCard('normal', '普通多开', '4 核 · 4 GB', '日常社交与应用均衡流畅')
              this.scenarioCard('performance', '性能模式', '8 核 · 6 GB', '大型应用与高帧率满血体验')
              this.scenarioCard('extreme', '极致多开', '2 核 · 2 GB', '精简资源占用，轻量独立环境')
            }
            .width('100%')
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)

          // 4. Graphics Engine, Refresh Rate & Launch Mode
          Column({ space: 12 }) {
            Row({ space: 12 }) {
              Text('渲染引擎')
                .fontSize(13)
                .fontColor($r('sys.color.font_secondary'))
                .width(72)
              Row({ space: 8 }) {
                this.segmentChip('Vulkan 加速', this.gpuMode === 'vulkan', () => { this.gpuMode = 'vulkan'; })
                this.segmentChip('OpenGL ES', this.gpuMode === 'gles', () => { this.gpuMode = 'gles'; })
                this.segmentChip('CPU 软渲', this.gpuMode === 'swiftshader', () => { this.gpuMode = 'swiftshader'; })
              }
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)

            Row({ space: 12 }) {
              Text('刷新率')
                .fontSize(13)
                .fontColor($r('sys.color.font_secondary'))
                .width(72)
              Row({ space: 8 }) {
                this.segmentChip('60 Hz', this.refreshRate === 60, () => { this.refreshRate = 60; })
                this.segmentChip('90 Hz', this.refreshRate === 90, () => { this.refreshRate = 90; })
                this.segmentChip('120 Hz', this.refreshRate === 120, () => { this.refreshRate = 120; })
              }
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)

            Row({ space: 12 }) {
              Text('窗口模式')
                .fontSize(13)
                .fontColor($r('sys.color.font_secondary'))
                .width(72)
              Row({ space: 8 }) {
                this.segmentChip('传统模拟器', this.runMode === 'emulator', () => { this.runMode = 'emulator'; })
                this.segmentChip('自由多窗', this.runMode === 'multiwindow', () => { this.runMode = 'multiwindow'; })
              }
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)
          }
          .width('100%')
          .padding(14)
          .borderRadius(14)
          .backgroundColor($r('sys.color.background_secondary'))

          // 5. Compact Disk Toggle
          Row() {
            Column({ space: 2 }) {
              Text('独立隔离数据分区（精简缓存模式）')
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_primary'))
              Text('每个设备拥有独立 /data 与 /sdcard 目录，互不干扰')
                .fontSize(11)
                .fontColor($r('sys.color.font_tertiary'))
            }
            .alignItems(HorizontalAlign.Start)
            Blank()
            Toggle({ type: ToggleType.Switch, isOn: this.compactDisk })
              .selectedColor($r('sys.color.brand'))
              .onChange((isOn: boolean) => {
                this.compactDisk = isOn;
              })
          }
          .width('100%')
          .padding({ left: 14, right: 14, top: 10, bottom: 10 })
          .borderRadius(14)
          .backgroundColor($r('sys.color.background_secondary'))
        }
        .padding({ left: 24, right: 24, top: 18, bottom: 18 })
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)

      Divider().color($r('sys.color.comp_divider')).opacity(0.4)

      // Footer
      Row({ space: 12 }) {
        Text(`将创建 ${this.count} 台独立 Android 14 设备`)
          .fontSize(12)
          .fontColor($r('sys.color.font_tertiary'))
        Blank()
        Button('取消', { type: ButtonType.Capsule })
          .height(38)
          .padding({ left: 22, right: 22 })
          .fontSize(14)
          .fontColor($r('sys.color.font_primary'))
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .onClick(() => {
            this.controller.close();
          })

        Button('立即创建', { type: ButtonType.Capsule })
          .height(38)
          .padding({ left: 24, right: 24 })
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .onClick(() => {
            this.onConfirm({
              deviceType: this.deviceType,
              androidVersion: 'Android 14',
              count: this.count,
              scenario: this.scenario,
              compactDisk: this.compactDisk,
              namePrefix: this.namePrefix,
              gpuMode: this.gpuMode,
              refreshRate: this.refreshRate,
              runMode: this.runMode,
            });
            this.controller.close();
          })
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 14, bottom: 18 })
    }
    .width(600)
    .height(580)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .borderRadius(24)
    .shadow({ radius: 32, color: '#26000000', offsetY: 8 })
  }
}
