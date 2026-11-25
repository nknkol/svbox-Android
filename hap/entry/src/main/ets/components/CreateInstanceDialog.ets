import { LengthUnit } from '@kit.ArkUI';
import {
  ControlSize,
  FontSize,
  IconSize,
  Motion,
  Radius,
  Spacing
} from '../common/DesignTokens';
import { CreateInstanceOptions } from '../model/InstanceModel';

@Component
export struct CreateInstanceSheet {
  compactLayout: boolean = false;
  onConfirm: (opts: CreateInstanceOptions) => void = (): void => {};
  onCancel: () => void = (): void => {};

  @State private deviceType: string = 'phone';
  @State private count: number = 1;
  @State private scenario: string = 'normal';
  @State private gpuMode: string = 'vulkan';
  @State private refreshRate: number = 120;
  @State private runMode: string = 'emulator';
  @State private compactDisk: boolean = false;
  @State private namePrefix: string = '';

  private animateSelection(fn: () => void): void {
    try {
      this.getUIContext().animateTo({
        duration: Motion.DUR_SHORT,
        curve: Motion.SPRING_SNAPPY
      }, fn);
    } catch (_e) {
      fn();
    }
  }

  @Builder
  private typeCard(key: string, title: string, desc: string, iconRes: Resource) {
    Row({ space: Spacing.MD }) {
      Stack({ alignContent: Alignment.Center }) {
        Column()
          .width(ControlSize.BTN_MD)
          .height(ControlSize.BTN_MD)
          .borderRadius(Radius.MD)
          .backgroundColor(
            this.deviceType === key
              ? $r('sys.color.comp_emphasize_secondary')
              : $r('sys.color.comp_background_tertiary')
          )
          .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
        SymbolGlyph(iconRes)
          .fontSize(IconSize.MD)
          .fontColor([
            this.deviceType === key
              ? $r('sys.color.icon_emphasize')
              : $r('sys.color.icon_secondary')
          ])
      }
      Column({ space: Spacing.XS }) {
        Text(title)
          .fontSize(FontSize.BODY_LG)
          .fontWeight(FontWeight.Medium)
          .fontColor(
            this.deviceType === key
              ? $r('sys.color.font_emphasize')
              : $r('sys.color.font_primary')
          )
        Text(desc)
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_secondary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
    }
    .layoutWeight(1)
    .padding(Spacing.MD)
    .borderRadius(Radius.CARD)
    .backgroundColor(
      this.deviceType === key
        ? $r('sys.color.comp_emphasize_tertiary')
        : $r('sys.color.comp_background_primary')
    )
    .border({
      width: this.deviceType === key ? 1.5 : 0.6,
      color: this.deviceType === key ? $r('sys.color.brand') : $r('sys.color.comp_divider')
    })
    .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY })
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.97 })
    .onClick(() => {
      this.animateSelection(() => {
        this.deviceType = key;
      });
    })
  }

  @Builder
  private scenarioCard(key: string, title: string, spec: string, desc: string) {
    Column({ space: Spacing.XS }) {
      Row() {
        Text(title)
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor(
            this.scenario === key
              ? $r('sys.color.font_emphasize')
              : $r('sys.color.font_primary')
          )
        Blank()
        if (this.scenario === key) {
          SymbolGlyph($r('sys.symbol.checkmark'))
            .fontSize(IconSize.XS)
            .fontColor([$r('sys.color.icon_emphasize')])
            .transition(Motion.pillPopTransition())
        }
      }
      .width('100%')

      Text(spec)
        .fontSize(FontSize.MICRO)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_secondary'))
      Text(desc)
        .fontSize(FontSize.MICRO)
        .fontColor($r('sys.color.font_tertiary'))
        .maxLines(2)
    }
    .width(this.compactLayout ? '100%' : undefined)
    .layoutWeight(this.compactLayout ? 0 : 1)
    .padding(Spacing.MD)
    .alignItems(HorizontalAlign.Start)
    .borderRadius(Radius.CARD)
    .backgroundColor(
      this.scenario === key
        ? $r('sys.color.comp_emphasize_tertiary')
        : $r('sys.color.comp_background_primary')
    )
    .border({
      width: this.scenario === key ? 1.5 : 0.6,
      color: this.scenario === key ? $r('sys.color.brand') : $r('sys.color.comp_divider')
    })
    .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY })
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.97 })
    .onClick(() => {
      this.animateSelection(() => {
        this.scenario = key;
      });
    })
  }

  @Builder
  private segmentChip(label: string, active: boolean, onTap: () => void) {
    Button({ type: ButtonType.Normal }) {
      Text(label)
        .fontSize(FontSize.CAPTION)
        .fontWeight(active ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(active ? $r('sys.color.font_on_primary') : $r('sys.color.font_primary'))
    }
    .height(ControlSize.BTN_SM)
    .padding({ left: Spacing.MD, right: Spacing.MD })
    .borderRadius(Radius.CAPSULE)
    .backgroundColor(active ? $r('sys.color.brand') : $r('sys.color.comp_background_tertiary'))
    .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
    .onClick(() => {
      this.animateSelection(onTap);
    })
  }

  @Builder
  private countStepper() {
    Row({ space: Spacing.SM }) {
      Button({ type: ButtonType.Normal }) {
        SymbolGlyph($r('sys.symbol.minus'))
          .fontSize(IconSize.SM)
          .fontColor([this.count > 1 ? $r('sys.color.icon_primary') : $r('sys.color.icon_fourth')])
      }
      .width(ControlSize.BTN_SM)
      .height(ControlSize.BTN_SM)
      .padding(0)
      .borderRadius(Radius.SM)
      .backgroundColor($r('sys.color.comp_background_tertiary'))
      .enabled(this.count > 1)
      .hoverEffect(HoverEffect.Highlight)
      .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
      .onClick(() => {
        if (this.count > 1) this.count -= 1;
      })

      Text(`${this.count}`)
        .fontSize(FontSize.BODY_LG)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_primary'))
        .width(24)
        .textAlign(TextAlign.Center)

      Button({ type: ButtonType.Normal }) {
        SymbolGlyph($r('sys.symbol.plus'))
          .fontSize(IconSize.SM)
          .fontColor([this.count < 5 ? $r('sys.color.icon_primary') : $r('sys.color.icon_fourth')])
      }
      .width(ControlSize.BTN_SM)
      .height(ControlSize.BTN_SM)
      .padding(0)
      .borderRadius(Radius.SM)
      .backgroundColor($r('sys.color.comp_background_tertiary'))
      .enabled(this.count < 5)
      .hoverEffect(HoverEffect.Highlight)
      .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
      .onClick(() => {
        if (this.count < 5) this.count += 1;
      })
    }
    .height(ControlSize.INPUT_H)
    .alignItems(VerticalAlign.Center)
  }

  build() {
    Column({ space: 0 }) {
      Scroll() {
        Column({ space: Spacing.LG }) {
          // 1. Device Type
          Column({ space: Spacing.SM }) {
            Text('设备形态')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))
            Row({ space: Spacing.MD }) {
              this.typeCard('phone', '手机', '720×1280 竖屏', $r('sys.symbol.phone_1'))
              this.typeCard('tablet', '平板', '1600×900 横屏', $r('sys.symbol.pad_fill'))
            }
            .width('100%')
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)

          // 2. Android Image + Count + Name (responsive)
          if (this.compactLayout) {
            Column({ space: Spacing.MD }) {
              Row({ space: Spacing.MD }) {
                Column({ space: Spacing.SM }) {
                  Text('系统镜像')
                    .fontSize(FontSize.BODY_MD)
                    .fontWeight(FontWeight.Medium)
                    .fontColor($r('sys.color.font_secondary'))
                  Row({ space: Spacing.SM }) {
                    Text('Android 14')
                      .fontSize(FontSize.BODY_MD)
                      .fontWeight(FontWeight.Medium)
                      .fontColor($r('sys.color.font_emphasize'))
                    Text('API 34')
                      .fontSize(FontSize.MICRO)
                      .fontColor($r('sys.color.font_secondary'))
                  }
                  .height(ControlSize.INPUT_H)
                  .padding({ left: Spacing.MD, right: Spacing.MD })
                  .borderRadius(Radius.MD)
                  .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
                  .border({ width: 1, color: $r('sys.color.brand') })
                }
                .alignItems(HorizontalAlign.Start)

                Blank()

                Column({ space: Spacing.SM }) {
                  Text('创建数量')
                    .fontSize(FontSize.BODY_MD)
                    .fontWeight(FontWeight.Medium)
                    .fontColor($r('sys.color.font_secondary'))
                  this.countStepper()
                }
                .alignItems(HorizontalAlign.Start)
              }
              .width('100%')

              Column({ space: Spacing.SM }) {
                Text('设备名称（可选）')
                  .fontSize(FontSize.BODY_MD)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_secondary'))
                TextInput({ placeholder: '例如：工作环境 / 测试机', text: this.namePrefix })
                  .width('100%')
                  .height(ControlSize.INPUT_H)
                  .fontSize(FontSize.BODY_MD)
                  .fontColor($r('sys.color.font_primary'))
                  .placeholderColor($r('sys.color.font_tertiary'))
                  .backgroundColor($r('sys.color.comp_background_primary'))
                  .borderRadius(Radius.MD)
                  .border({ width: 0.6, color: $r('sys.color.comp_divider') })
                  .maxLength(32)
                  .onChange((val: string) => {
                    this.namePrefix = val;
                  })
              }
              .width('100%')
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
          } else {
            Row({ space: Spacing.LG }) {
              Column({ space: Spacing.SM }) {
                Text('系统镜像')
                  .fontSize(FontSize.BODY_MD)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_secondary'))
                Row({ space: Spacing.SM }) {
                  Text('Android 14')
                    .fontSize(FontSize.BODY_MD)
                    .fontWeight(FontWeight.Medium)
                    .fontColor($r('sys.color.font_emphasize'))
                  Text('API 34')
                    .fontSize(FontSize.MICRO)
                    .fontColor($r('sys.color.font_secondary'))
                }
                .height(ControlSize.INPUT_H)
                .padding({ left: Spacing.MD, right: Spacing.MD })
                .borderRadius(Radius.MD)
                .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
                .border({ width: 1, color: $r('sys.color.brand') })
              }
              .alignItems(HorizontalAlign.Start)

              Column({ space: Spacing.SM }) {
                Text('创建数量')
                  .fontSize(FontSize.BODY_MD)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_secondary'))
                this.countStepper()
              }
              .alignItems(HorizontalAlign.Start)

              Column({ space: Spacing.SM }) {
                Text('设备名称（可选）')
                  .fontSize(FontSize.BODY_MD)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_secondary'))
                TextInput({ placeholder: '例如：工作环境 / 测试机', text: this.namePrefix })
                  .height(ControlSize.INPUT_H)
                  .fontSize(FontSize.BODY_MD)
                  .fontColor($r('sys.color.font_primary'))
                  .placeholderColor($r('sys.color.font_tertiary'))
                  .backgroundColor($r('sys.color.comp_background_primary'))
                  .borderRadius(Radius.MD)
                  .border({ width: 0.6, color: $r('sys.color.comp_divider') })
                  .maxLength(32)
                  .onChange((val: string) => {
                    this.namePrefix = val;
                  })
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
          }

          // 3. Scenario Presets
          Column({ space: Spacing.SM }) {
            Text('性能场景')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))
            if (this.compactLayout) {
              Column({ space: Spacing.SM }) {
                this.scenarioCard('normal', '普通多开', '4 核 · 4 GB', '日常应用均衡流畅')
                this.scenarioCard('performance', '性能模式', '8 核 · 6 GB', '高帧率满血体验')
                this.scenarioCard('extreme', '极致多开', '2 核 · 2 GB', '轻量低占用环境')
              }
              .width('100%')
            } else {
              Row({ space: Spacing.MD }) {
                this.scenarioCard('normal', '普通多开', '4 核 · 4 GB', '日常应用均衡流畅')
                this.scenarioCard('performance', '性能模式', '8 核 · 6 GB', '高帧率满血体验')
                this.scenarioCard('extreme', '极致多开', '2 核 · 2 GB', '轻量低占用环境')
              }
              .width('100%')
            }
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)

          // 4. Graphics Engine, Refresh Rate & Launch Mode
          Column({ space: Spacing.MD }) {
            Row({ space: Spacing.MD }) {
              Text('渲染引擎')
                .fontSize(FontSize.BODY_MD)
                .fontColor($r('sys.color.font_secondary'))
                .width(64)
              Flex({
                wrap: FlexWrap.Wrap,
                space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
              }) {
                this.segmentChip('Vulkan 加速', this.gpuMode === 'vulkan', () => { this.gpuMode = 'vulkan'; })
                this.segmentChip('OpenGL ES', this.gpuMode === 'gles', () => { this.gpuMode = 'gles'; })
                this.segmentChip('CPU 软渲', this.gpuMode === 'swiftshader', () => { this.gpuMode = 'swiftshader'; })
              }
              .layoutWeight(1)
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)

            Row({ space: Spacing.MD }) {
              Text('屏幕帧率')
                .fontSize(FontSize.BODY_MD)
                .fontColor($r('sys.color.font_secondary'))
                .width(64)
              Flex({
                wrap: FlexWrap.Wrap,
                space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
              }) {
                this.segmentChip('60 Hz', this.refreshRate === 60, () => { this.refreshRate = 60; })
                this.segmentChip('90 Hz', this.refreshRate === 90, () => { this.refreshRate = 90; })
                this.segmentChip('120 Hz', this.refreshRate === 120, () => { this.refreshRate = 120; })
              }
              .layoutWeight(1)
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)

            Row({ space: Spacing.MD }) {
              Text('窗口模式')
                .fontSize(FontSize.BODY_MD)
                .fontColor($r('sys.color.font_secondary'))
                .width(64)
              Flex({
                wrap: FlexWrap.Wrap,
                space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
              }) {
                this.segmentChip('传统模拟器', this.runMode === 'emulator', () => { this.runMode = 'emulator'; })
                this.segmentChip('自由多窗', this.runMode === 'multiwindow', () => { this.runMode = 'multiwindow'; })
              }
              .layoutWeight(1)
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)
          }
          .width('100%')
          .padding(Spacing.LG)
          .borderRadius(Radius.CARD)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })

          // 5. Compact Disk Mode
          Row({ space: Spacing.MD }) {
            Column({ space: Spacing.XS }) {
              Text('独立隔离数据分区（精简缓存模式）')
                .fontSize(FontSize.BODY_MD)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_primary'))
              Text('每台设备均拥有独立 /data 与 /sdcard 目录，互不干扰')
                .fontSize(FontSize.MICRO)
                .fontColor($r('sys.color.font_tertiary'))
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            Toggle({ type: ToggleType.Switch, isOn: this.compactDisk })
              .selectedColor($r('sys.color.brand'))
              .onChange((isOn: boolean) => {
                this.compactDisk = isOn;
              })
          }
          .width('100%')
          .padding({ left: Spacing.LG, right: Spacing.LG, top: Spacing.MD, bottom: Spacing.MD })
          .borderRadius(Radius.CARD)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        }
        .width('100%')
        .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.SM, bottom: Spacing.LG })
      }
      .width('100%')
      .layoutWeight(1)
      .align(Alignment.TopStart)
      .scrollBar(BarState.Auto)
      .scrollBarWidth(3)
      .scrollBarColor($r('sys.color.comp_divider'))

      Divider().color($r('sys.color.comp_divider')).opacity(0.4)

      Row({ space: Spacing.MD }) {
        if (!this.compactLayout) {
          Text(`将创建 ${this.count} 台独立 Android 14 设备`)
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_tertiary'))
        }
        Blank()
        Button('取消', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.XL, right: Spacing.XL })
          .fontSize(FontSize.BODY_LG)
          .fontColor($r('sys.color.font_primary'))
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
          .onClick(() => {
            this.onCancel();
          })

        Button('立即创建', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.XXL, right: Spacing.XXL })
          .fontSize(FontSize.BODY_LG)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
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
          })
      }
      .width('100%')
      .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.MD, bottom: Spacing.LG })
    }
    .width('100%')
    .height('100%')
  }
}
