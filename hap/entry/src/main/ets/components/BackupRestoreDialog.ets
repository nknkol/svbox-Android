import native from 'libentry.so';
import {
  ControlSize,
  FontSize,
  IconSize,
  Motion,
  Radius,
  Spacing
} from '../common/DesignTokens';
import { DeviceInstance } from '../model/InstanceModel';

@Component
export struct BackupRestoreSheet {
  instance: DeviceInstance | undefined = undefined;
  filesDir: string = '';
  backupDir: string = '/storage/Users/currentUser/Download/svbox-backups';
  isInstanceRunning: boolean = false;
  onFinished: (msg: string) => void = (): void => {};
  onClose: () => void = (): void => {};

  @State private compressBackup: boolean = true;
  @State private busy: boolean = false;
  @State private busyText: string = '';
  @State private backupFiles: string[] = [];
  @State private selectedBackup: string = '';

  aboutToAppear(): void {
    native.ensureDir(this.backupDir);
    this.refreshBackups();
  }

  private refreshBackups(): void {
    try {
      this.backupFiles = native.listBackupFiles(this.backupDir);
      if (this.backupFiles.length > 0 && this.selectedBackup.length === 0) {
        this.selectedBackup = this.backupFiles[0];
      }
    } catch (_e) {
      this.backupFiles = [];
    }
  }

  private async runBackup(): Promise<void> {
    if (!this.instance || this.busy) return;
    this.busy = true;
    this.busyText = '正在打包备份设备数据，请稍候...';
    const d: Date = new Date();
    const stamp: string = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}_${String(d.getHours()).padStart(2, '0')}${String(d.getMinutes()).padStart(2, '0')}${String(d.getSeconds()).padStart(2, '0')}`;
    const ext: string = this.compressBackup ? 'tar.gz' : 'tar';
    const safeName: string = `svbox_inst${this.instance.index}_${stamp}.${ext}`;
    const outPath: string = `${this.backupDir}/${safeName}`;
    try {
      const res: string = await native.backupInstanceAsync(
        this.filesDir,
        this.instance.dataSubdir,
        outPath,
        this.compressBackup
      );
      if (res.startsWith('OK:')) {
        this.refreshBackups();
        this.onFinished(`备份完成：${safeName}`);
      } else {
        this.onFinished(`备份失败：${res}`);
      }
    } catch (e) {
      this.onFinished(`备份异常：${String(e)}`);
    } finally {
      this.busy = false;
      this.busyText = '';
    }
  }

  private async runRestore(): Promise<void> {
    if (!this.instance || this.busy || this.selectedBackup.length === 0) return;
    if (this.isInstanceRunning) {
      this.onFinished('请先关闭该设备后再执行还原');
      return;
    }
    this.busy = true;
    this.busyText = `正在从 ${this.selectedBackup} 还原数据...`;
    const inPath: string = `${this.backupDir}/${this.selectedBackup}`;
    try {
      const res: string = await native.restoreInstanceAsync(
        this.filesDir,
        this.instance.dataSubdir,
        inPath
      );
      if (res === 'OK') {
        this.onFinished(`已成功还原：${this.selectedBackup}`);
        this.onClose();
      } else {
        this.onFinished(`还原失败：${res}`);
      }
    } catch (e) {
      this.onFinished(`还原异常：${String(e)}`);
    } finally {
      this.busy = false;
      this.busyText = '';
    }
  }

  build() {
    Column({ space: Spacing.LG }) {
      // Backup Card
      Column({ space: Spacing.MD }) {
        Row({ space: Spacing.MD }) {
          Column({ space: Spacing.XS }) {
            Text('备份设备数据')
              .fontSize(FontSize.TITLE_SM)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text(`备份路径：${this.backupDir}`)
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_tertiary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .layoutWeight(1)
          .alignItems(HorizontalAlign.Start)

          Button('立即备份', { type: ButtonType.Capsule })
            .height(ControlSize.BTN_MD)
            .padding({ left: Spacing.LG, right: Spacing.LG })
            .fontSize(FontSize.BODY_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_on_primary'))
            .backgroundColor($r('sys.color.brand'))
            .enabled(!this.busy)
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
            .onClick(() => {
              this.runBackup();
            })
        }
        .width('100%')

        Row({ space: Spacing.SM }) {
          Checkbox()
            .select(this.compressBackup)
            .selectedColor($r('sys.color.brand'))
            .width(18)
            .height(18)
            .onChange((checked: boolean) => {
              this.compressBackup = checked;
            })
          Text('启用 gzip 压缩（占用空间更小，耗时略增加）')
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_secondary'))
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })

      // Restore Card
      Column({ space: Spacing.MD }) {
        Row({ space: Spacing.MD }) {
          Column({ space: Spacing.XS }) {
            Text('从已有备份还原')
              .fontSize(FontSize.TITLE_SM)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text(this.isInstanceRunning ? '设备正在运行中，请先停止后再还原' : '还原将覆盖当前设备的全部应用与数据')
              .fontSize(FontSize.MICRO)
              .fontColor(this.isInstanceRunning ? $r('sys.color.alert') : $r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Button('刷新列表', { type: ButtonType.Capsule })
            .height(ControlSize.BTN_SM)
            .padding({ left: Spacing.MD, right: Spacing.MD })
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_tertiary'))
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
            .onClick(() => {
              this.refreshBackups();
            })
        }
        .width('100%')

        if (this.backupFiles.length === 0) {
          Column({ space: Spacing.SM }) {
            SymbolGlyph($r('sys.symbol.archivebox'))
              .fontSize(IconSize.LG)
              .fontColor([$r('sys.color.icon_tertiary')])
            Text('暂无历史备份文件')
              .fontSize(FontSize.BODY_MD)
              .fontColor($r('sys.color.font_tertiary'))
          }
          .width('100%')
          .height(128)
          .justifyContent(FlexAlign.Center)
        } else {
          List({ space: Spacing.SM }) {
            ForEach(
              this.backupFiles,
              (fname: string) => {
                ListItem() {
                  Row({ space: Spacing.MD }) {
                    SymbolGlyph($r('sys.symbol.archivebox'))
                      .fontSize(IconSize.SM)
                      .fontColor([
                        this.selectedBackup === fname
                          ? $r('sys.color.icon_emphasize')
                          : $r('sys.color.icon_secondary')
                      ])
                    Text(fname)
                      .fontSize(FontSize.BODY_MD)
                      .fontColor(
                        this.selectedBackup === fname
                          ? $r('sys.color.font_emphasize')
                          : $r('sys.color.font_primary')
                      )
                      .layoutWeight(1)
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                    if (this.selectedBackup === fname) {
                      SymbolGlyph($r('sys.symbol.checkmark'))
                        .fontSize(IconSize.SM)
                        .fontColor([$r('sys.color.icon_emphasize')])
                        .transition(Motion.pillPopTransition())
                    }
                  }
                  .width('100%')
                  .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.SM, bottom: Spacing.SM })
                  .borderRadius(Radius.MD)
                  .backgroundColor(
                    this.selectedBackup === fname
                      ? $r('sys.color.comp_emphasize_tertiary')
                      : $r('sys.color.background_secondary')
                  )
                  .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
                  .hoverEffect(HoverEffect.Highlight)
                  .onClick(() => {
                    this.selectedBackup = fname;
                  })
                }
              },
              (fname: string): string => fname
            )
          }
          .width('100%')
          .height(140)
        }

        Row() {
          Blank()
          Button('还原所选备份', { type: ButtonType.Capsule })
            .height(ControlSize.BTN_MD)
            .padding({ left: Spacing.LG, right: Spacing.LG })
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_on_primary'))
            .backgroundColor($r('sys.color.warning'))
            .enabled(!this.busy && !this.isInstanceRunning && this.selectedBackup.length > 0)
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
            .onClick(() => {
              this.runRestore();
            })
        }
        .width('100%')
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })

      if (this.busy) {
        Row({ space: Spacing.SM }) {
          LoadingProgress()
            .width(IconSize.MD)
            .height(IconSize.MD)
            .color($r('sys.color.brand'))
          Text(this.busyText)
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_secondary'))
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .transition(Motion.pillPopTransition())
      }
    }
    .width('100%')
    .height('100%')
    .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.SM, bottom: Spacing.XL })
  }
}
