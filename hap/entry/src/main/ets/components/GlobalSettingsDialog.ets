import native from 'libentry.so';
import {
  ControlSize,
  FontSize,
  IconSize,
  Motion,
  Radius,
  Spacing
} from '../common/DesignTokens';
import { GlobalSettings } from '../model/InstanceModel';

@Component
export struct GlobalSettingsSheet {
  initialSettings: GlobalSettings | undefined = undefined;
  compactLayout: boolean = false;
  onSave: (next: GlobalSettings) => void = (): void => {};
  onCancel: () => void = (): void => {};

  @State private autoBootDefault: boolean = false;
  @State private batchStartIntervalSec: number = 3;
  @State private windowArrangeMode: string = 'grid';
  @State private backupDir: string = '/storage/Users/currentUser/Download/svbox-backups';
  @State private sortBy: string = 'index';
  @State private viewMode: string = 'card';
  @State private dirStatusHint: string = '';

  aboutToAppear(): void {
    if (this.initialSettings) {
      this.autoBootDefault = this.initialSettings.autoBootDefault;
      this.batchStartIntervalSec = this.initialSettings.batchStartIntervalSec;
      this.windowArrangeMode = this.initialSettings.windowArrangeMode;
      this.backupDir = this.initialSettings.backupDir;
      this.sortBy = this.initialSettings.sortBy;
      this.viewMode = this.initialSettings.viewMode;
    }
  }

  private animateSelect(fn: () => void): void {
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
  private arrangeModeCard(key: string, title: string, desc: string) {
    Column({ space: Spacing.XS }) {
      Text(title)
        .fontSize(FontSize.BODY_MD)
        .fontWeight(FontWeight.Medium)
        .fontColor(
          this.windowArrangeMode === key
            ? $r('sys.color.font_emphasize')
            : $r('sys.color.font_primary')
        )
      Text(desc)
        .fontSize(FontSize.MICRO)
        .fontColor($r('sys.color.font_tertiary'))
    }
    .width(this.compactLayout ? '100%' : undefined)
    .layoutWeight(this.compactLayout ? 0 : 1)
    .padding(Spacing.MD)
    .alignItems(HorizontalAlign.Start)
    .borderRadius(Radius.MD)
    .backgroundColor(
      this.windowArrangeMode === key
        ? $r('sys.color.comp_emphasize_tertiary')
        : $r('sys.color.background_secondary')
    )
    .border({
      width: this.windowArrangeMode === key ? 1.5 : 0.6,
      color: this.windowArrangeMode === key ? $r('sys.color.brand') : $r('sys.color.comp_divider')
    })
    .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY })
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.97 })
    .onClick(() => {
      this.animateSelect(() => { this.windowArrangeMode = key; });
    })
  }

  build() {
    Column({ space: 0 }) {
      Scroll() {
        Column({ space: Spacing.MD }) {
          // Startup & Multi-instance behavior
          Column({ space: Spacing.MD }) {
            Text('启动与多开')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))

            Row({ space: Spacing.MD }) {
              Column({ space: Spacing.XS }) {
                Text('启动应用时自动开机默认设备')
                  .fontSize(FontSize.BODY_LG)
                  .fontColor($r('sys.color.font_primary'))
                Text('关闭后仅打开设备管理面板，按需启动指定实例')
                  .fontSize(FontSize.MICRO)
                  .fontColor($r('sys.color.font_tertiary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Toggle({ type: ToggleType.Switch, isOn: this.autoBootDefault })
                .selectedColor($r('sys.color.brand'))
                .onChange((isOn: boolean) => {
                  this.autoBootDefault = isOn;
                })
            }
            .width('100%')

            Divider().color($r('sys.color.comp_divider')).opacity(0.35)

            Row({ space: Spacing.MD }) {
              Column({ space: Spacing.XS }) {
                Text('批量操作间隔（秒）')
                  .fontSize(FontSize.BODY_LG)
                  .fontColor($r('sys.color.font_primary'))
                Text('避免短时间内触发过高 CPU / IO 峰值')
                  .fontSize(FontSize.MICRO)
                  .fontColor($r('sys.color.font_tertiary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row({ space: Spacing.SM }) {
                Button({ type: ButtonType.Normal }) {
                  SymbolGlyph($r('sys.symbol.minus'))
                    .fontSize(IconSize.SM)
                    .fontColor([
                      this.batchStartIntervalSec > 1
                        ? $r('sys.color.icon_primary')
                        : $r('sys.color.icon_fourth')
                    ])
                }
                .width(ControlSize.BTN_SM)
                .height(ControlSize.BTN_SM)
                .padding(0)
                .borderRadius(Radius.SM)
                .backgroundColor($r('sys.color.comp_background_tertiary'))
                .enabled(this.batchStartIntervalSec > 1)
                .hoverEffect(HoverEffect.Highlight)
                .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
                .onClick(() => {
                  if (this.batchStartIntervalSec > 1) this.batchStartIntervalSec -= 1;
                })

                Text(`${this.batchStartIntervalSec} s`)
                  .fontSize(FontSize.BODY_MD)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_primary'))
                  .width(36)
                  .textAlign(TextAlign.Center)

                Button({ type: ButtonType.Normal }) {
                  SymbolGlyph($r('sys.symbol.plus'))
                    .fontSize(IconSize.SM)
                    .fontColor([
                      this.batchStartIntervalSec < 15
                        ? $r('sys.color.icon_primary')
                        : $r('sys.color.icon_fourth')
                    ])
                }
                .width(ControlSize.BTN_SM)
                .height(ControlSize.BTN_SM)
                .padding(0)
                .borderRadius(Radius.SM)
                .backgroundColor($r('sys.color.comp_background_tertiary'))
                .enabled(this.batchStartIntervalSec < 15)
                .hoverEffect(HoverEffect.Highlight)
                .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
                .onClick(() => {
                  if (this.batchStartIntervalSec < 15) this.batchStartIntervalSec += 1;
                })
              }
              .alignItems(VerticalAlign.Center)
            }
            .width('100%')
          }
          .width('100%')
          .padding(Spacing.LG)
          .borderRadius(Radius.CARD)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .alignItems(HorizontalAlign.Start)

          // Window Arrangement
          Column({ space: Spacing.MD }) {
            Text('窗口排列方式')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))

            if (this.compactLayout) {
              Column({ space: Spacing.SM }) {
                this.arrangeModeCard('grid', '网格平铺（并排）', '按窗口宽度从左向右自动并排对齐')
                this.arrangeModeCard('cascade', '对角层叠排列', '沿对角线错开排列，方便快速切换窗口')
              }
              .width('100%')
            } else {
              Row({ space: Spacing.MD }) {
                this.arrangeModeCard('grid', '网格平铺（并排）', '按窗口宽度从左向右自动并排对齐')
                this.arrangeModeCard('cascade', '对角层叠排列', '沿对角线错开排列，方便快速切换窗口')
              }
              .width('100%')
            }
          }
          .width('100%')
          .padding(Spacing.LG)
          .borderRadius(Radius.CARD)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .alignItems(HorizontalAlign.Start)

          // Backup Directory
          Column({ space: Spacing.MD }) {
            Text('数据与备份目录')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))

            Row({ space: Spacing.SM }) {
              TextInput({ text: this.backupDir })
                .layoutWeight(1)
                .height(ControlSize.INPUT_H)
                .fontSize(FontSize.CAPTION)
                .fontColor($r('sys.color.font_primary'))
                .backgroundColor($r('sys.color.background_secondary'))
                .borderRadius(Radius.MD)
                .onChange((val: string) => {
                  this.backupDir = val;
                })
              Button('检查目录', { type: ButtonType.Capsule })
                .height(ControlSize.INPUT_H)
                .padding({ left: Spacing.LG, right: Spacing.LG })
                .fontSize(FontSize.CAPTION)
                .fontColor($r('sys.color.font_primary'))
                .backgroundColor($r('sys.color.comp_background_tertiary'))
                .hoverEffect(HoverEffect.Highlight)
                .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
                .onClick(() => {
                  const ok: boolean = native.ensureDir(this.backupDir);
                  this.dirStatusHint = ok ? '目录已就绪' : '无法创建目录，请检查路径权限';
                })
            }
            .width('100%')

            if (this.dirStatusHint.length > 0) {
              Text(this.dirStatusHint)
                .fontSize(FontSize.MICRO)
                .fontColor($r('sys.color.confirm'))
                .transition(Motion.pillPopTransition())
            }
          }
          .width('100%')
          .padding(Spacing.LG)
          .borderRadius(Radius.CARD)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .alignItems(HorizontalAlign.Start)
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

        Button('保存设置', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.XXL, right: Spacing.XXL })
          .fontSize(FontSize.BODY_LG)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
          .onClick(() => {
            this.onSave({
              autoBootDefault: this.autoBootDefault,
              batchStartIntervalSec: this.batchStartIntervalSec,
              windowArrangeMode: this.windowArrangeMode,
              backupDir: this.backupDir.trim().length > 0
                ? this.backupDir.trim()
                : '/storage/Users/currentUser/Download/svbox-backups',
              sortBy: this.sortBy,
              viewMode: this.viewMode,
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
