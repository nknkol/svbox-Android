import native from 'libentry.so';
import { GlobalSettings } from '../model/InstanceModel';

@CustomDialog
export struct GlobalSettingsDialog {
  controller: CustomDialogController;
  initialSettings: GlobalSettings | undefined = undefined;
  onSave: (next: GlobalSettings) => void = (): void => {};

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

  build() {
    Column({ space: 0 }) {
      // Header
      Row() {
        Row({ space: 8 }) {
          SymbolGlyph($r('sys.symbol.gearshape_fill'))
            .fontSize(20)
            .fontColor([$r('sys.color.icon_emphasize')])
          Text('设置中心')
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
        Column({ space: 16 }) {
          // Startup & Multi-instance behavior
          Column({ space: 12 }) {
            Text('启动与多开')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))

            Row() {
              Column({ space: 2 }) {
                Text('启动应用时自动开机默认设备')
                  .fontSize(14)
                  .fontColor($r('sys.color.font_primary'))
                Text('关闭后仅打开设备管理面板，按需启动指定实例')
                  .fontSize(11)
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

            Row() {
              Column({ space: 2 }) {
                Text('批量操作间隔（秒）')
                  .fontSize(14)
                  .fontColor($r('sys.color.font_primary'))
                Text('避免短时间内触发过高 CPU / IO 峰值')
                  .fontSize(11)
                  .fontColor($r('sys.color.font_tertiary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row({ space: 8 }) {
                Button('−', { type: ButtonType.Normal })
                  .width(30)
                  .height(30)
                  .borderRadius(8)
                  .fontColor($r('sys.color.font_primary'))
                  .backgroundColor($r('sys.color.comp_background_tertiary'))
                  .enabled(this.batchStartIntervalSec > 1)
                  .onClick(() => {
                    if (this.batchStartIntervalSec > 1) this.batchStartIntervalSec -= 1;
                  })
                Text(`${this.batchStartIntervalSec} s`)
                  .fontSize(13)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_primary'))
                  .width(36)
                  .textAlign(TextAlign.Center)
                Button('+', { type: ButtonType.Normal })
                  .width(30)
                  .height(30)
                  .borderRadius(8)
                  .fontColor($r('sys.color.font_primary'))
                  .backgroundColor($r('sys.color.comp_background_tertiary'))
                  .enabled(this.batchStartIntervalSec < 15)
                  .onClick(() => {
                    if (this.batchStartIntervalSec < 15) this.batchStartIntervalSec += 1;
                  })
              }
            }
            .width('100%')
          }
          .width('100%')
          .padding(16)
          .borderRadius(16)
          .backgroundColor($r('sys.color.background_secondary'))
          .alignItems(HorizontalAlign.Start)

          // Window Arrangement
          Column({ space: 12 }) {
            Text('窗口排列方式')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))

            Row({ space: 10 }) {
              Column({ space: 4 }) {
                Text('网格平铺（并排）')
                  .fontSize(13)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(
                    this.windowArrangeMode === 'grid'
                      ? $r('sys.color.font_emphasize')
                      : $r('sys.color.font_primary')
                  )
                Text('按窗口宽度从左向右自动并排对齐')
                  .fontSize(11)
                  .fontColor($r('sys.color.font_tertiary'))
              }
              .layoutWeight(1)
              .padding(12)
              .alignItems(HorizontalAlign.Start)
              .borderRadius(12)
              .backgroundColor(
                this.windowArrangeMode === 'grid'
                  ? $r('sys.color.comp_emphasize_tertiary')
                  : $r('sys.color.comp_background_primary')
              )
              .border({
                width: this.windowArrangeMode === 'grid' ? 1.5 : 1,
                color: this.windowArrangeMode === 'grid' ? $r('sys.color.brand') : Color.Transparent
              })
              .onClick(() => {
                this.windowArrangeMode = 'grid';
              })

              Column({ space: 4 }) {
                Text('对角层叠排列')
                  .fontSize(13)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(
                    this.windowArrangeMode === 'cascade'
                      ? $r('sys.color.font_emphasize')
                      : $r('sys.color.font_primary')
                  )
                Text('沿对角线错开排列，方便快速切换窗口')
                  .fontSize(11)
                  .fontColor($r('sys.color.font_tertiary'))
              }
              .layoutWeight(1)
              .padding(12)
              .alignItems(HorizontalAlign.Start)
              .borderRadius(12)
              .backgroundColor(
                this.windowArrangeMode === 'cascade'
                  ? $r('sys.color.comp_emphasize_tertiary')
                  : $r('sys.color.comp_background_primary')
              )
              .border({
                width: this.windowArrangeMode === 'cascade' ? 1.5 : 1,
                color: this.windowArrangeMode === 'cascade' ? $r('sys.color.brand') : Color.Transparent
              })
              .onClick(() => {
                this.windowArrangeMode = 'cascade';
              })
            }
            .width('100%')
          }
          .width('100%')
          .padding(16)
          .borderRadius(16)
          .backgroundColor($r('sys.color.background_secondary'))
          .alignItems(HorizontalAlign.Start)

          // Backup Directory
          Column({ space: 10 }) {
            Text('数据与备份目录')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))

            Row({ space: 10 }) {
              TextInput({ text: this.backupDir })
                .layoutWeight(1)
                .height(36)
                .fontSize(12)
                .fontColor($r('sys.color.font_primary'))
                .backgroundColor($r('sys.color.comp_background_primary'))
                .borderRadius(10)
                .onChange((val: string) => {
                  this.backupDir = val;
                })
              Button('检查目录', { type: ButtonType.Normal })
                .height(36)
                .padding({ left: 14, right: 14 })
                .borderRadius(10)
                .fontSize(12)
                .fontColor($r('sys.color.font_primary'))
                .backgroundColor($r('sys.color.comp_background_tertiary'))
                .onClick(() => {
                  const ok: boolean = native.ensureDir(this.backupDir);
                  this.dirStatusHint = ok ? '目录已就绪' : '无法创建目录，请检查路径权限';
                })
            }
            .width('100%')

            if (this.dirStatusHint.length > 0) {
              Text(this.dirStatusHint)
                .fontSize(11)
                .fontColor($r('sys.color.confirm'))
            }
          }
          .width('100%')
          .padding(16)
          .borderRadius(16)
          .backgroundColor($r('sys.color.background_secondary'))
          .alignItems(HorizontalAlign.Start)
        }
        .padding({ left: 24, right: 24, top: 18, bottom: 18 })
      }
      .layoutWeight(1)

      Divider().color($r('sys.color.comp_divider')).opacity(0.4)

      Row({ space: 12 }) {
        Text('svbox Android · HarmonyOS PC Edition')
          .fontSize(12)
          .fontColor($r('sys.color.font_tertiary'))
        Blank()
        Button('取消', { type: ButtonType.Capsule })
          .height(36)
          .padding({ left: 20, right: 20 })
          .fontSize(14)
          .fontColor($r('sys.color.font_primary'))
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .onClick(() => {
            this.controller.close();
          })

        Button('保存设置', { type: ButtonType.Capsule })
          .height(36)
          .padding({ left: 22, right: 22 })
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
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
            this.controller.close();
          })
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 14, bottom: 18 })
    }
    .width(560)
    .height(500)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .borderRadius(24)
    .shadow({ radius: 32, color: '#26000000', offsetY: 8 })
  }
}
