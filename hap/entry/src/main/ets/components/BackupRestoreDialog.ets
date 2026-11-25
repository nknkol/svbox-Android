import native from 'libentry.so';
import { DeviceInstance } from '../model/InstanceModel';

@CustomDialog
export struct BackupRestoreDialog {
  controller: CustomDialogController;
  instance: DeviceInstance | undefined = undefined;
  filesDir: string = '';
  backupDir: string = '/storage/Users/currentUser/Download/svbox-backups';
  isInstanceRunning: boolean = false;
  onFinished: (msg: string) => void = (): void => {};

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
        this.controller.close();
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
    Column({ space: 0 }) {
      // Header
      Row() {
        Row({ space: 8 }) {
          SymbolGlyph($r('sys.symbol.archivebox'))
            .fontSize(20)
            .fontColor([$r('sys.color.icon_emphasize')])
          Column({ space: 2 }) {
            Text('备份与还原')
              .fontSize(18)
              .fontWeight(FontWeight.Bold)
              .fontColor($r('sys.color.font_primary'))
            Text(this.instance ? `${this.instance.name} (#${this.instance.index})` : '')
              .fontSize(12)
              .fontColor($r('sys.color.font_secondary'))
          }
          .alignItems(HorizontalAlign.Start)
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
        .enabled(!this.busy)
        .onClick(() => {
          this.controller.close();
        })
      }
      .width('100%')
      .padding({ left: 24, right: 20, top: 20, bottom: 14 })

      Divider().color($r('sys.color.comp_divider')).opacity(0.4)

      Column({ space: 16 }) {
        // Backup Card
        Column({ space: 12 }) {
          Row() {
            Column({ space: 3 }) {
              Text('备份设备数据')
                .fontSize(15)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_primary'))
              Text(`备份至：${this.backupDir}`)
                .fontSize(11)
                .fontColor($r('sys.color.font_tertiary'))
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
            }
            .layoutWeight(1)
            .alignItems(HorizontalAlign.Start)

            Button('立即备份', { type: ButtonType.Capsule })
              .height(34)
              .padding({ left: 18, right: 18 })
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_on_primary'))
              .backgroundColor($r('sys.color.brand'))
              .enabled(!this.busy)
              .onClick(() => {
                this.runBackup();
              })
          }
          .width('100%')

          Row({ space: 8 }) {
            Checkbox()
              .select(this.compressBackup)
              .selectedColor($r('sys.color.brand'))
              .width(18)
              .height(18)
              .onChange((checked: boolean) => {
                this.compressBackup = checked;
              })
            Text('启用 gzip 压缩（占用空间更小，耗时略增加）')
              .fontSize(12)
              .fontColor($r('sys.color.font_secondary'))
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .padding(16)
        .borderRadius(16)
        .backgroundColor($r('sys.color.background_secondary'))

        // Restore Card
        Column({ space: 10 }) {
          Row() {
            Column({ space: 2 }) {
              Text('从已有备份还原')
                .fontSize(15)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_primary'))
              Text(this.isInstanceRunning ? '设备正在运行中，请先停止后再还原' : '还原将覆盖当前设备的全部应用与数据')
                .fontSize(11)
                .fontColor(this.isInstanceRunning ? $r('sys.color.alert') : $r('sys.color.font_tertiary'))
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            Button('刷新列表', { type: ButtonType.Normal })
              .height(30)
              .padding({ left: 12, right: 12 })
              .borderRadius(8)
              .fontSize(12)
              .fontColor($r('sys.color.font_primary'))
              .backgroundColor($r('sys.color.comp_background_tertiary'))
              .onClick(() => {
                this.refreshBackups();
              })
          }
          .width('100%')

          if (this.backupFiles.length === 0) {
            Column({ space: 6 }) {
              Text('暂无历史备份文件')
                .fontSize(13)
                .fontColor($r('sys.color.font_tertiary'))
            }
            .width('100%')
            .height(92)
            .justifyContent(FlexAlign.Center)
          } else {
            List({ space: 6 }) {
              ForEach(
                this.backupFiles,
                (fname: string) => {
                  ListItem() {
                    Row({ space: 10 }) {
                      SymbolGlyph($r('sys.symbol.archivebox'))
                        .fontSize(15)
                        .fontColor([
                          this.selectedBackup === fname
                            ? $r('sys.color.icon_emphasize')
                            : $r('sys.color.icon_secondary')
                        ])
                      Text(fname)
                        .fontSize(13)
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
                          .fontSize(14)
                          .fontColor([$r('sys.color.icon_emphasize')])
                      }
                    }
                    .width('100%')
                    .padding({ left: 12, right: 12, top: 9, bottom: 9 })
                    .borderRadius(10)
                    .backgroundColor(
                      this.selectedBackup === fname
                        ? $r('sys.color.comp_emphasize_tertiary')
                        : $r('sys.color.comp_background_primary')
                    )
                    .onClick(() => {
                      this.selectedBackup = fname;
                    })
                  }
                },
                (fname: string): string => fname
              )
            }
            .width('100%')
            .height(120)
          }

          Row() {
            Blank()
            Button('还原所选备份', { type: ButtonType.Capsule })
              .height(34)
              .padding({ left: 18, right: 18 })
              .fontSize(13)
              .fontColor($r('sys.color.font_on_primary'))
              .backgroundColor($r('sys.color.warning'))
              .enabled(!this.busy && !this.isInstanceRunning && this.selectedBackup.length > 0)
              .onClick(() => {
                this.runRestore();
              })
          }
          .width('100%')
        }
        .width('100%')
        .padding(16)
        .borderRadius(16)
        .backgroundColor($r('sys.color.background_secondary'))

        if (this.busy) {
          Row({ space: 10 }) {
            LoadingProgress()
              .width(20)
              .height(20)
              .color($r('sys.color.brand'))
            Text(this.busyText)
              .fontSize(12)
              .fontColor($r('sys.color.font_secondary'))
          }
          .width('100%')
          .justifyContent(FlexAlign.Center)
        }
      }
      .padding(24)
    }
    .width(520)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .borderRadius(24)
    .shadow({ radius: 32, color: '#26000000', offsetY: 8 })
  }
}
