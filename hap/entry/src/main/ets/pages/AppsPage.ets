import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native from 'libentry.so';
import {
  AndroidAppItem,
  AppWindowPref,
  appWindowPrefs,
  computeShortcutId
} from '../common/AppWindowPrefs';
import {
  pinAndroidAppToDesktop,
  queryPinnedShortcutIds,
  removeAndroidAppShortcut,
  ShortcutOpResult
} from '../common/DesktopShortcut';

const TAG: string = 'SvboxAppsPage';

interface ListAppsReply {
  ok?: boolean;
  apps?: AndroidAppItem[];
}

@Entry
@Component
struct AppsPage {
  @StorageLink('mainDecorRightAvoidVp') decorRightAvoidVp: number = 148;
  @StorageLink('androidApps') appList: AndroidAppItem[] = [];
  @StorageLink('refreshRate') refreshRate: number = 120;

  @State private wmAgentReady: boolean = false;
  @State private running: boolean = false;
  @State private iconsDir: string = '';
  @State private loadingApps: boolean = false;
  @State private searchQuery: string = '';
  @State private appFilter: string = 'all';
  @State private pinnedIds: string[] = [];
  @State private prefEpoch: number = 0;

  private pollTimer: number = -1;

  private showToast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
    } catch (_e) {
    }
  }

  private getSandboxFilesDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxFilesDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/files';
  }

  aboutToAppear(): void {
    this.iconsDir = native.getIconsDir();
    this.wmAgentReady = native.isWmAgentReady();
    this.running = native.getStatus().running;
    this.refreshPinnedShortcuts();
    if (this.wmAgentReady && this.appList.length === 0) {
      this.refreshApps(false);
    }
    this.pollTimer = setInterval(() => {
      try {
        const st = native.getStatus();
        this.running = st.running;
        this.wmAgentReady = st.wmAgentReady;
        if (this.iconsDir.length === 0) {
          this.iconsDir = native.getIconsDir();
        }
        if (this.wmAgentReady && this.appList.length === 0 && !this.loadingApps) {
          this.refreshApps(false);
        }
      } catch (_e) {
      }
    }, 600);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  private async refreshApps(forceExtractIcons: boolean): Promise<void> {
    if (!this.wmAgentReady || this.loadingApps) return;
    this.loadingApps = true;
    try {
      this.iconsDir = native.getIconsDir();
      const raw: string = await native.wmRpcAsync(forceExtractIcons ? 'LIST_APPS 1' : 'LIST_APPS 0');
      const rep: ListAppsReply = JSON.parse(raw) as ListAppsReply;
      if (rep.ok && rep.apps) {
        this.appList = rep.apps;
        AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', rep.apps);
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'refreshApps error: %{public}s', String(err));
    } finally {
      this.loadingApps = false;
    }
  }

  private async refreshPinnedShortcuts(): Promise<void> {
    try {
      this.pinnedIds = await queryPinnedShortcutIds();
    } catch (_e) {
    }
  }

  private isAppPinned(pkg: string): boolean {
    return this.pinnedIds.indexOf(computeShortcutId(pkg)) >= 0;
  }

  private getFilteredApps(): AndroidAppItem[] {
    const q: string = this.searchQuery.trim().toLowerCase();
    const out: AndroidAppItem[] = [];
    for (let i: number = 0; i < this.appList.length; i++) {
      const a: AndroidAppItem = this.appList[i];
      if (this.appFilter === 'user' && a.isSystem) continue;
      if (this.appFilter === 'system' && !a.isSystem) continue;
      if (q.length > 0) {
        const hay: string = `${a.label} ${a.packageName}`.toLowerCase();
        if (hay.indexOf(q) < 0) continue;
      }
      out.push(a);
    }
    return out;
  }

  private getEffectiveModeLabel(app: AndroidAppItem): string {
    const pref: AppWindowPref = appWindowPrefs.getPref(
      this.getSandboxFilesDir(), app.packageName, app, this.refreshRate
    );
    if (pref.mode === 'adaptive') return '自由';
    if (pref.mode === 'fixed_landscape') return '横屏';
    return '竖屏';
  }

  private cycleAppWindowPref(app: AndroidAppItem): void {
    const dir: string = this.getSandboxFilesDir();
    const cur: AppWindowPref = appWindowPrefs.getPref(dir, app.packageName, app, this.refreshRate);
    let nextMode: string = 'adaptive';
    let nextPreset: string = cur.presetKey;
    let nextW: number = cur.winW;
    let nextH: number = cur.winH;
    if (cur.mode === 'adaptive') {
      nextMode = 'fixed_portrait';
      nextPreset = '720x1280';
      nextW = 860;
      nextH = 1380;
    } else if (cur.mode === 'fixed_portrait') {
      nextMode = 'fixed_landscape';
      nextPreset = '1280x720';
      nextW = 1480;
      nextH = 940;
    } else {
      nextMode = 'adaptive';
    }
    appWindowPrefs.savePref(dir, app.packageName, {
      mode: nextMode,
      presetKey: nextPreset,
      refreshRate: cur.refreshRate,
      winW: nextW,
      winH: nextH,
    });
    this.prefEpoch += 1;
    const lbl: string = nextMode === 'adaptive' ? '自由窗口' : (nextMode === 'fixed_landscape' ? '固定横屏' : '固定竖屏');
    this.showToast(`${app.label}：已设为${lbl}`);
  }

  private async toggleDesktopShortcut(app: AndroidAppItem): Promise<void> {
    const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
    if (this.isAppPinned(app.packageName)) {
      const r: ShortcutOpResult = await removeAndroidAppShortcut(ctx, app.packageName);
      this.showToast(r.message);
      this.refreshPinnedShortcuts();
      return;
    }
    const iconPath: string = this.iconsDir.length > 0 && app.iconFile.length > 0
      ? `${this.iconsDir}/${app.iconFile}` : '';
    const r: ShortcutOpResult = await pinAndroidAppToDesktop(ctx, app, iconPath);
    this.showToast(r.message);
    this.refreshPinnedShortcuts();
  }

  private async openAndroidAppWindow(app: AndroidAppItem): Promise<void> {
    try {
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        abilityName: 'AppWindowAbility',
        parameters: {
          'androidPkg': app.packageName,
          'androidComp': app.component,
          'androidLabel': app.label.length > 0 ? app.label : app.packageName,
          'moveTaskId': '-1',
        },
      };
      await ctx.startAbility(want);
    } catch (err) {
      hilog.error(0x0000, TAG, 'openAndroidAppWindow failed: %{public}s', String(err));
      this.showToast(`打开失败: ${String(err)}`);
    }
  }

  build() {
    Column({ space: 0 }) {
      // HarmonyOS PC Adaptive Title Bar
      Row({ space: 10 }) {
        Button({ type: ButtonType.Capsule }) {
          Row({ space: 4 }) {
            SymbolGlyph($r('sys.symbol.chevron_left'))
              .fontSize(14)
              .fontColor([$r('sys.color.icon_primary')])
            Text('设备管理')
              .fontSize(13)
              .fontColor($r('sys.color.font_primary'))
          }
        }
        .height(32)
        .padding({ left: 10, right: 12 })
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .onClick(() => {
          this.getUIContext().getRouter().back();
        })

        Row({ space: 6 }) {
          SymbolGlyph($r('sys.symbol.square_grid_2x2'))
            .fontSize(16)
            .fontColor([$r('sys.color.icon_emphasize')])
          Text('应用中心')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_primary'))
        }

        Text(`${this.appList.length} 个应用`)
          .fontSize(11)
          .fontColor($r('sys.color.font_secondary'))
          .padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .borderRadius(8)
          .backgroundColor($r('sys.color.background_secondary'))

        Blank()

        Row({ space: 5 }) {
          SymbolGlyph($r('sys.symbol.pin_fill'))
            .fontSize(12)
            .fontColor([$r('sys.color.icon_emphasize')])
          Text(`桌面 ${this.pinnedIds.length}/10`)
            .fontSize(11)
            .fontColor($r('sys.color.font_emphasize'))
        }
        .padding({ left: 10, right: 10, top: 5, bottom: 5 })
        .borderRadius(12)
        .backgroundColor($r('sys.color.comp_emphasize_tertiary'))

        Button({ type: ButtonType.Capsule }) {
          Row({ space: 5 }) {
            SymbolGlyph($r('sys.symbol.arrow_clockwise'))
              .fontSize(13)
              .fontColor([$r('sys.color.icon_primary')])
            Text('刷新')
              .fontSize(12)
              .fontColor($r('sys.color.font_primary'))
          }
        }
        .height(30)
        .padding({ left: 12, right: 12 })
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .enabled(this.wmAgentReady && !this.loadingApps)
        .onClick(() => {
          this.refreshApps(true);
        })
      }
      .width('100%')
      .height(48)
      .padding({ left: 16, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: { bottom: 0.6 }, color: $r('sys.color.comp_divider') })
      .alignItems(VerticalAlign.Center)

      Column({ space: 14 }) {
        Row({ space: 12 }) {
          Row({ space: 8 }) {
            SymbolGlyph($r('sys.symbol.magnifyingglass'))
              .fontSize(15)
              .fontColor([$r('sys.color.icon_tertiary')])
            TextInput({ placeholder: '搜索 Android 应用名称或包名...', text: this.searchQuery })
              .layoutWeight(1)
              .height(34)
              .fontSize(13)
              .fontColor($r('sys.color.font_primary'))
              .placeholderColor($r('sys.color.font_tertiary'))
              .backgroundColor(Color.Transparent)
              .padding({ left: 0, right: 0 })
              .onChange((val: string) => {
                this.searchQuery = val;
              })
          }
          .layoutWeight(1)
          .height(38)
          .padding({ left: 14, right: 12 })
          .borderRadius(19)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .alignItems(VerticalAlign.Center)

          Row({ space: 2 }) {
            Button('全部', { type: ButtonType.Normal })
              .height(30)
              .padding({ left: 12, right: 12 })
              .borderRadius(15)
              .fontSize(12)
              .fontColor(this.appFilter === 'all' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
              .backgroundColor(this.appFilter === 'all' ? $r('sys.color.brand') : Color.Transparent)
              .onClick(() => { this.appFilter = 'all'; })
            Button('第三方', { type: ButtonType.Normal })
              .height(30)
              .padding({ left: 12, right: 12 })
              .borderRadius(15)
              .fontSize(12)
              .fontColor(this.appFilter === 'user' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
              .backgroundColor(this.appFilter === 'user' ? $r('sys.color.brand') : Color.Transparent)
              .onClick(() => { this.appFilter = 'user'; })
            Button('系统', { type: ButtonType.Normal })
              .height(30)
              .padding({ left: 12, right: 12 })
              .borderRadius(15)
              .fontSize(12)
              .fontColor(this.appFilter === 'system' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
              .backgroundColor(this.appFilter === 'system' ? $r('sys.color.brand') : Color.Transparent)
              .onClick(() => { this.appFilter = 'system'; })
          }
          .padding(3)
          .borderRadius(18)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        }
        .width('100%')

        if (!this.wmAgentReady) {
          Column({ space: 12 }) {
            LoadingProgress()
              .width(36)
              .height(36)
              .color($r('sys.color.brand'))
            Text(this.running ? 'Android 14 正在启动，应用列表即将就绪...' : '请先在设备管理中启动 Android 14')
              .fontSize(14)
              .fontColor($r('sys.color.font_secondary'))
          }
          .width('100%')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
        } else if (this.getFilteredApps().length === 0) {
          Column({ space: 8 }) {
            Text(this.loadingApps ? '正在同步应用与图标...' : '暂无匹配应用')
              .fontSize(14)
              .fontColor($r('sys.color.font_secondary'))
          }
          .width('100%')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
        } else {
          Grid() {
            ForEach(
              this.getFilteredApps(),
              (app: AndroidAppItem) => {
                GridItem() {
                  Column({ space: 10 }) {
                    Column({ space: 8 }) {
                      if (this.iconsDir.length > 0 && app.iconFile.length > 0) {
                        Image(`file://${this.iconsDir}/${app.iconFile}`)
                          .width(54)
                          .height(54)
                          .borderRadius(14)
                          .objectFit(ImageFit.Contain)
                      } else {
                        Row() {
                          Text(app.label.length > 0 ? app.label.substring(0, 1).toUpperCase() : 'A')
                            .fontSize(22)
                            .fontWeight(FontWeight.Bold)
                            .fontColor($r('sys.color.font_on_primary'))
                        }
                        .width(54)
                        .height(54)
                        .borderRadius(14)
                        .backgroundColor($r('sys.color.brand'))
                        .justifyContent(FlexAlign.Center)
                        .alignItems(VerticalAlign.Center)
                      }

                      Text(app.label)
                        .fontSize(13)
                        .fontWeight(FontWeight.Medium)
                        .fontColor($r('sys.color.font_primary'))
                        .maxLines(1)
                        .textOverflow({ overflow: TextOverflow.Ellipsis })
                        .textAlign(TextAlign.Center)
                        .width('100%')
                    }
                    .width('100%')
                    .alignItems(HorizontalAlign.Center)
                    .onClick(() => {
                      this.openAndroidAppWindow(app);
                    })

                    Row({ space: 6 }) {
                      Button({ type: ButtonType.Normal }) {
                        Row({ space: 3 }) {
                          SymbolGlyph($r('sys.symbol.screen_rotation'))
                            .fontSize(11)
                            .fontColor([$r('sys.color.icon_secondary')])
                          Text(this.getEffectiveModeLabel(app))
                            .fontSize(10)
                            .fontColor($r('sys.color.font_secondary'))
                        }
                      }
                      .height(24)
                      .padding({ left: 8, right: 8 })
                      .borderRadius(12)
                      .backgroundColor($r('sys.color.background_secondary'))
                      .onClick(() => { this.cycleAppWindowPref(app); })

                      Button({ type: ButtonType.Normal }) {
                        Row({ space: 3 }) {
                          SymbolGlyph(this.isAppPinned(app.packageName) ? $r('sys.symbol.pin_fill') : $r('sys.symbol.pin'))
                            .fontSize(11)
                            .fontColor([
                              this.isAppPinned(app.packageName)
                                ? $r('sys.color.confirm')
                                : $r('sys.color.icon_emphasize')
                            ])
                          Text(this.isAppPinned(app.packageName) ? '已加桌' : '加桌')
                            .fontSize(10)
                            .fontColor(
                              this.isAppPinned(app.packageName)
                                ? $r('sys.color.confirm')
                                : $r('sys.color.font_emphasize')
                            )
                        }
                      }
                      .height(24)
                      .padding({ left: 8, right: 8 })
                      .borderRadius(12)
                      .backgroundColor(
                        this.isAppPinned(app.packageName)
                          ? '#1F64BB5C'
                          : $r('sys.color.comp_emphasize_tertiary')
                      )
                      .onClick(() => { this.toggleDesktopShortcut(app); })
                    }
                    .justifyContent(FlexAlign.Center)
                  }
                  .width('100%')
                  .padding({ left: 10, right: 10, top: 14, bottom: 12 })
                  .borderRadius(16)
                  .backgroundColor($r('sys.color.comp_background_primary'))
                  .border({ width: 0.6, color: $r('sys.color.comp_divider') })
                }
              },
              (app: AndroidAppItem): string => `${app.packageName}_${this.prefEpoch}_${this.pinnedIds.length}`
            )
          }
          .columnsTemplate('1fr 1fr 1fr 1fr 1fr 1fr')
          .columnsGap(12)
          .rowsGap(12)
          .width('100%')
          .layoutWeight(1)
        }
      }
      .width('100%')
      .layoutWeight(1)
      .padding({ left: 20, right: 20, top: 16, bottom: 14 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('sys.color.background_secondary'))
  }
}
