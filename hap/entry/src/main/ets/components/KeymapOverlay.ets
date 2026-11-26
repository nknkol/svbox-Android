import native from 'libentry.so';
import { FontSize, IconSize, Radius, Spacing } from '../common/DesignTokens';
import {
  createFpsPreset,
  createGeneralPreset,
  createMobaPreset,
  KeyChoice,
  keyCodeToLabel,
  KeymapItem,
  KeymapProfile,
  MacroRunner,
  MacroScript,
  MacroStep,
  macroStore,
  QUICK_KEY_CHOICES
} from '../common/KeymapStore';

@Component
export struct KeymapHintLayer {
  @Prop items: KeymapItem[] = [];
  @Prop viewW: number = 400;
  @Prop viewH: number = 700;

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      ForEach(this.items, (item: KeymapItem) => {
        if (item.kind === 'dpad') {
          Stack({ alignContent: Alignment.Center }) {
            Circle()
              .width(Math.max(64, Math.round(item.radius * 2 * Math.min(this.viewW, this.viewH))))
              .height(Math.max(64, Math.round(item.radius * 2 * Math.min(this.viewW, this.viewH))))
              .fill('#28000000')
              .stroke('#88FFFFFF')
              .strokeWidth(1.5)
            Column({ space: 2 }) {
              Text(keyCodeToLabel(item.upKey))
                .fontSize(10)
                .fontWeight(FontWeight.Bold)
                .fontColor('#DDFFFFFF')
              Row({ space: 18 }) {
                Text(keyCodeToLabel(item.leftKey))
                  .fontSize(10)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#DDFFFFFF')
                Text(keyCodeToLabel(item.rightKey))
                  .fontSize(10)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#DDFFFFFF')
              }
              Text(keyCodeToLabel(item.downKey))
                .fontSize(10)
                .fontWeight(FontWeight.Bold)
                .fontColor('#DDFFFFFF')
            }
          }
          .position({
            x: Math.round(item.nx * this.viewW - Math.max(32, item.radius * Math.min(this.viewW, this.viewH))),
            y: Math.round(item.ny * this.viewH - Math.max(32, item.radius * Math.min(this.viewW, this.viewH))),
          })
        } else if (item.kind === 'rmb_move') {
          Row({ space: 4 }) {
            Text('RMB 移动')
              .fontSize(10)
              .fontWeight(FontWeight.Medium)
              .fontColor('#EEFFFFFF')
          }
          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
          .borderRadius(12)
          .backgroundColor('#660A59F7')
          .border({ width: 1, color: '#AAFFFFFF' })
          .position({
            x: Math.round(item.nx * this.viewW - 34),
            y: Math.round(item.ny * this.viewH - 12),
          })
        } else {
          Stack({ alignContent: Alignment.Center }) {
            Circle()
              .width(34)
              .height(34)
              .fill('#55000000')
              .stroke('#99FFFFFF')
              .strokeWidth(1.2)
            Text(item.label)
              .fontSize(item.label.length > 2 ? 9 : 12)
              .fontWeight(FontWeight.Bold)
              .fontColor('#EEFFFFFF')
          }
          .position({
            x: Math.round(item.nx * this.viewW - 17),
            y: Math.round(item.ny * this.viewH - 17),
          })
        }
      }, (item: KeymapItem) => `${item.id}_${item.nx.toFixed(3)}_${item.ny.toFixed(3)}_${item.label}`)
    }
    .width(this.viewW)
    .height(this.viewH)
    .hitTestBehavior(HitTestMode.None)
  }
}

@Component
export struct KeymapEditorOverlay {
  @Prop pkg: string = '';
  @Prop viewW: number = 400;
  @Prop viewH: number = 700;
  @Prop initialProfile: KeymapProfile = { pkg: '', enabled: true, showHints: true, items: [] };
  onSave: (profile: KeymapProfile) => void = () => {};
  onClose: () => void = () => {};

  @State private items: KeymapItem[] = [];
  @State private showHints: boolean = true;
  @State private selectedId: string = '';
  private dragStartNx: number = 0.5;
  private dragStartNy: number = 0.5;

  aboutToAppear(): void {
    this.showHints = this.initialProfile.showHints;
    this.items = this.initialProfile.items.map((it: KeymapItem): KeymapItem => ({
      id: it.id,
      kind: it.kind,
      label: it.label,
      keyCode: it.keyCode,
      upKey: it.upKey,
      downKey: it.downKey,
      leftKey: it.leftKey,
      rightKey: it.rightKey,
      nx: it.nx,
      ny: it.ny,
      radius: it.radius,
      direct: it.direct,
    }));
    if (this.items.length > 0) {
      this.selectedId = this.items[0].id;
    }
  }

  private getSelected(): KeymapItem | undefined {
    for (let i: number = 0; i < this.items.length; i++) {
      if (this.items[i].id === this.selectedId) return this.items[i];
    }
    return undefined;
  }

  private updateItemPos(id: string, nx: number, ny: number): void {
    const cNx: number = Math.max(0.03, Math.min(0.97, nx));
    const cNy: number = Math.max(0.05, Math.min(0.95, ny));
    this.items = this.items.map((it: KeymapItem): KeymapItem => {
      if (it.id !== id) return it;
      return {
        id: it.id,
        kind: it.kind,
        label: it.label,
        keyCode: it.keyCode,
        upKey: it.upKey,
        downKey: it.downKey,
        leftKey: it.leftKey,
        rightKey: it.rightKey,
        nx: cNx,
        ny: cNy,
        radius: it.radius,
        direct: it.direct,
      };
    });
  }

  private updateSelectedKey(choice: KeyChoice): void {
    if (this.selectedId.length === 0) return;
    this.items = this.items.map((it: KeymapItem): KeymapItem => {
      if (it.id !== this.selectedId || it.kind !== 'tap') return it;
      return {
        id: it.id,
        kind: it.kind,
        label: choice.label,
        keyCode: choice.code,
        upKey: it.upKey,
        downKey: it.downKey,
        leftKey: it.leftKey,
        rightKey: it.rightKey,
        nx: it.nx,
        ny: it.ny,
        radius: it.radius,
        direct: it.direct,
      };
    });
  }

  private addDpad(): void {
    const exists: boolean = this.items.some((it: KeymapItem): boolean => it.kind === 'dpad');
    if (exists) return;
    const item: KeymapItem = {
      id: `dpad_${Date.now()}`,
      kind: 'dpad',
      label: 'WASD',
      keyCode: 0,
      upKey: 2039,
      downKey: 2035,
      leftKey: 2017,
      rightKey: 2020,
      nx: 0.18,
      ny: 0.74,
      radius: 0.11,
      direct: false,
    };
    this.items = [...this.items, item];
    this.selectedId = item.id;
  }

  private addTap(): void {
    const used: number[] = this.items.map((it: KeymapItem): number => it.keyCode);
    let pick: KeyChoice = QUICK_KEY_CHOICES[0];
    for (let i: number = 0; i < QUICK_KEY_CHOICES.length; i++) {
      if (used.indexOf(QUICK_KEY_CHOICES[i].code) < 0) {
        pick = QUICK_KEY_CHOICES[i];
        break;
      }
    }
    const item: KeymapItem = {
      id: `tap_${Date.now()}_${this.items.length}`,
      kind: 'tap',
      label: pick.label,
      keyCode: pick.code,
      upKey: 0,
      downKey: 0,
      leftKey: 0,
      rightKey: 0,
      nx: 0.50 + ((this.items.length % 4) - 1.5) * 0.08,
      ny: 0.50,
      radius: 0.05,
      direct: true,
    };
    this.items = [...this.items, item];
    this.selectedId = item.id;
  }

  private addRmbMove(): void {
    const exists: boolean = this.items.some((it: KeymapItem): boolean => it.kind === 'rmb_move');
    if (exists) return;
    const item: KeymapItem = {
      id: `rmb_${Date.now()}`,
      kind: 'rmb_move',
      label: '右键移动',
      keyCode: 0,
      upKey: 0,
      downKey: 0,
      leftKey: 0,
      rightKey: 0,
      nx: 0.18,
      ny: 0.74,
      radius: 0.11,
      direct: false,
    };
    this.items = [...this.items, item];
    this.selectedId = item.id;
  }

  private removeSelected(): void {
    if (this.selectedId.length === 0) return;
    this.items = this.items.filter((it: KeymapItem): boolean => it.id !== this.selectedId);
    this.selectedId = this.items.length > 0 ? this.items[0].id : '';
  }

  private applyPreset(preset: KeymapProfile): void {
    this.items = preset.items;
    this.showHints = true;
    this.selectedId = this.items.length > 0 ? this.items[0].id : '';
  }

  private commitSave(): void {
    const out: KeymapProfile = {
      pkg: this.pkg.length > 0 ? this.pkg : '__emulator__',
      enabled: this.items.length > 0,
      showHints: this.showHints,
      items: this.items,
    };
    this.onSave(out);
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      // Dim backdrop to make draggable badges stand out clearly
      Column()
        .width(this.viewW)
        .height(this.viewH)
        .backgroundColor('#33000000')

      // Draggable key badges
      ForEach(this.items, (item: KeymapItem) => {
        if (item.kind === 'dpad') {
          Stack({ alignContent: Alignment.Center }) {
            Circle()
              .width(Math.max(72, Math.round(item.radius * 2 * Math.min(this.viewW, this.viewH))))
              .height(Math.max(72, Math.round(item.radius * 2 * Math.min(this.viewW, this.viewH))))
              .fill(this.selectedId === item.id ? '#660A59F7' : '#44000000')
              .stroke(this.selectedId === item.id ? '#FF409EFF' : '#CCFFFFFF')
              .strokeWidth(2)
            Text('WASD\n拖拽定位')
              .fontSize(11)
              .fontWeight(FontWeight.Bold)
              .fontColor('#FFFFFF')
              .textAlign(TextAlign.Center)
          }
          .position({
            x: Math.round(item.nx * this.viewW - Math.max(36, item.radius * Math.min(this.viewW, this.viewH))),
            y: Math.round(item.ny * this.viewH - Math.max(36, item.radius * Math.min(this.viewW, this.viewH))),
          })
          .onClick(() => { this.selectedId = item.id; })
          .gesture(
            PanGesture()
              .onActionStart(() => {
                this.selectedId = item.id;
                this.dragStartNx = item.nx;
                this.dragStartNy = item.ny;
              })
              .onActionUpdate((ev: GestureEvent) => {
                this.updateItemPos(
                  item.id,
                  this.dragStartNx + ev.offsetX / Math.max(1, this.viewW),
                  this.dragStartNy + ev.offsetY / Math.max(1, this.viewH)
                );
              })
          )
        } else if (item.kind === 'rmb_move') {
          Row({ space: 4 }) {
            Text('右键走位 · 拖拽中心')
              .fontSize(11)
              .fontWeight(FontWeight.Bold)
              .fontColor('#FFFFFF')
          }
          .padding({ left: 10, right: 10, top: 6, bottom: 6 })
          .borderRadius(14)
          .backgroundColor(this.selectedId === item.id ? '#CC0A59F7' : '#880A59F7')
          .border({ width: 1.5, color: '#FFFFFF' })
          .position({
            x: Math.round(item.nx * this.viewW - 54),
            y: Math.round(item.ny * this.viewH - 14),
          })
          .onClick(() => { this.selectedId = item.id; })
          .gesture(
            PanGesture()
              .onActionStart(() => {
                this.selectedId = item.id;
                this.dragStartNx = item.nx;
                this.dragStartNy = item.ny;
              })
              .onActionUpdate((ev: GestureEvent) => {
                this.updateItemPos(
                  item.id,
                  this.dragStartNx + ev.offsetX / Math.max(1, this.viewW),
                  this.dragStartNy + ev.offsetY / Math.max(1, this.viewH)
                );
              })
          )
        } else {
          Stack({ alignContent: Alignment.Center }) {
            Circle()
              .width(42)
              .height(42)
              .fill(this.selectedId === item.id ? '#CC0A59F7' : '#88000000')
              .stroke(this.selectedId === item.id ? '#FFFFFF' : '#CCFFFFFF')
              .strokeWidth(2)
            Text(item.label)
              .fontSize(item.label.length > 2 ? 10 : 14)
              .fontWeight(FontWeight.Bold)
              .fontColor('#FFFFFF')
          }
          .position({
            x: Math.round(item.nx * this.viewW - 21),
            y: Math.round(item.ny * this.viewH - 21),
          })
          .onClick(() => { this.selectedId = item.id; })
          .gesture(
            PanGesture()
              .onActionStart(() => {
                this.selectedId = item.id;
                this.dragStartNx = item.nx;
                this.dragStartNy = item.ny;
              })
              .onActionUpdate((ev: GestureEvent) => {
                this.updateItemPos(
                  item.id,
                  this.dragStartNx + ev.offsetX / Math.max(1, this.viewW),
                  this.dragStartNy + ev.offsetY / Math.max(1, this.viewH)
                );
              })
          )
        }
      }, (item: KeymapItem) => `${item.id}_${item.label}_${item.nx.toFixed(3)}_${item.ny.toFixed(3)}`)

      // Floating Top Control Strip
      Scroll() {
        Row({ space: Spacing.XS }) {
          Button('+ WASD', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD1E2430').fontColor('#FFFFFF')
            .onClick(() => { this.addDpad(); })
          Button('+ 点击键', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD1E2430').fontColor('#FFFFFF')
            .onClick(() => { this.addTap(); })
          Button('+ 右键走位', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD1E2430').fontColor('#FFFFFF')
            .onClick(() => { this.addRmbMove(); })
          Button('MOBA 模板', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD2B3A55').fontColor('#FFFFFF')
            .onClick(() => { this.applyPreset(createMobaPreset(this.pkg)); })
          Button('FPS 模板', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD2B3A55').fontColor('#FFFFFF')
            .onClick(() => { this.applyPreset(createFpsPreset(this.pkg)); })
          Button('通用模板', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD2B3A55').fontColor('#FFFFFF')
            .onClick(() => { this.applyPreset(createGeneralPreset(this.pkg)); })
          Button(this.showHints ? '键位提示:开' : '键位提示:关', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DD1E2430').fontColor('#FFFFFF')
            .onClick(() => { this.showHints = !this.showHints; })
          Button('清空', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#DDE84026').fontColor('#FFFFFF')
            .onClick(() => { this.items = []; this.selectedId = ''; })
          Button('保存生效', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#FF0A59F7').fontColor('#FFFFFF')
            .onClick(() => { this.commitSave(); })
          Button('取消', { type: ButtonType.Capsule })
            .height(28).fontSize(FontSize.MICRO).backgroundColor('#CC333333').fontColor('#FFFFFF')
            .onClick(() => { this.onClose(); })
        }
        .padding({ left: Spacing.SM, right: Spacing.SM })
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .width(this.viewW)
      .padding({ top: Spacing.SM })

      // Bottom Key Picker for Selected Tap Item
      if (this.selectedId.length > 0) {
        Column({ space: Spacing.XS }) {
          Row({ space: Spacing.SM }) {
            Text(this.getSelected()
              ? `已选：${this.getSelected()!.label} (${Math.round(this.getSelected()!.nx * 100)}%, ${Math.round(this.getSelected()!.ny * 100)}%)`
              : '')
              .fontSize(FontSize.MICRO)
              .fontColor('#FFFFFF')
              .layoutWeight(1)
            Button('删除键位', { type: ButtonType.Capsule })
              .height(24).fontSize(11).backgroundColor('#DDE84026').fontColor('#FFFFFF')
              .onClick(() => { this.removeSelected(); })
          }
          .width('100%')

          if (this.getSelected()?.kind === 'tap') {
            Scroll() {
              Row({ space: 4 }) {
                ForEach(QUICK_KEY_CHOICES, (ch: KeyChoice) => {
                  Button(ch.label, { type: ButtonType.Capsule })
                    .height(26)
                    .fontSize(11)
                    .fontColor('#FFFFFF')
                    .backgroundColor(this.getSelected()?.keyCode === ch.code ? '#FF0A59F7' : '#CC2A3140')
                    .onClick(() => { this.updateSelectedKey(ch); })
                }, (ch: KeyChoice) => String(ch.code))
              }
            }
            .scrollable(ScrollDirection.Horizontal)
            .scrollBar(BarState.Off)
            .width('100%')
          }
        }
        .width(this.viewW)
        .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.XS, bottom: Spacing.SM })
        .backgroundColor('#CC141822')
        .position({ x: 0, y: Math.max(44, this.viewH - 76) })
      }
    }
    .width(this.viewW)
    .height(this.viewH)
  }
}

@Component
export struct MacroAutomationOverlay {
  @Prop filesDir: string = '';
  @Prop pkg: string = '';
  @Prop displaySlot: number = 0;
  @Prop viewW: number = 400;
  @Prop viewH: number = 700;
  onClose: () => void = () => {};

  @State private scripts: MacroScript[] = [];
  @State private recording: boolean = false;
  @State private recordedSteps: MacroStep[] = [];
  @State private draftName: string = '连点/挂机宏';
  @State private loopCount: number = 1;
  @State private runningId: string = '';
  @State private progressText: string = '';

  private runner: MacroRunner = new MacroRunner();
  private lastRecTs: number = 0;
  private downNx: number = 0;
  private downNy: number = 0;
  private downTs: number = 0;

  aboutToAppear(): void {
    this.reloadScripts();
  }

  aboutToDisappear(): void {
    this.runner.stop(this.displaySlot);
  }

  private reloadScripts(): void {
    if (this.filesDir.length === 0) return;
    this.scripts = macroStore.getForPackage(this.filesDir, this.pkg);
  }

  private startRecording(): void {
    this.runner.stop(this.displaySlot);
    this.runningId = '';
    this.recordedSteps = [];
    this.lastRecTs = Date.now();
    this.recording = true;
    this.progressText = '录制中：直接点击或滑动屏幕，操作将同步作用于 Android 并记录步骤';
  }

  private saveRecording(): void {
    this.recording = false;
    if (this.recordedSteps.length === 0) {
      this.progressText = '未录制到操作步骤';
      return;
    }
    const sc: MacroScript = {
      id: `macro_${Date.now()}`,
      name: this.draftName.trim().length > 0 ? this.draftName.trim() : `脚本 ${this.scripts.length + 1}`,
      pkg: this.pkg.length > 0 ? this.pkg : '__emulator__',
      loopCount: this.loopCount,
      intervalMs: 400,
      steps: this.recordedSteps,
    };
    macroStore.upsert(this.filesDir, sc);
    this.recordedSteps = [];
    this.reloadScripts();
    this.progressText = `已保存「${sc.name}」(${sc.steps.length} 步)`;
  }

  private toggleRunScript(sc: MacroScript): void {
    if (this.runningId === sc.id) {
      this.runner.stop(this.displaySlot);
      this.runningId = '';
      this.progressText = '已停止脚本';
      return;
    }
    this.runningId = sc.id;
    const runCopy: MacroScript = {
      id: sc.id,
      name: sc.name,
      pkg: sc.pkg,
      loopCount: this.loopCount,
      intervalMs: sc.intervalMs,
      steps: sc.steps,
    };
    this.runner.run(
      this.displaySlot,
      runCopy,
      (txt: string) => { this.progressText = txt; },
      () => {
        this.runningId = '';
        this.progressText = '执行完成';
      }
    );
  }

  private deleteScript(id: string): void {
    if (this.runningId === id) {
      this.runner.stop(this.displaySlot);
      this.runningId = '';
    }
    macroStore.remove(this.filesDir, id);
    this.reloadScripts();
  }

  private handleRecordTouch(ev: TouchEvent): void {
    if (!this.recording || ev.touches.length === 0) return;
    const t: TouchObject = ev.touches[0];
    const nx: number = Math.max(0.01, Math.min(0.99, t.x / Math.max(1, this.viewW)));
    const ny: number = Math.max(0.01, Math.min(0.99, t.y / Math.max(1, this.viewH)));
    const now: number = Date.now();

    if (ev.type === TouchType.Down) {
      const gap: number = now - this.lastRecTs;
      if (this.recordedSteps.length > 0 && gap >= 120) {
        const waitStep: MacroStep = {
          id: `st_${now}_w`,
          kind: 'wait',
          label: `等待 ${Math.min(10000, gap)}ms`,
          nx: 0,
          ny: 0,
          nx2: 0,
          ny2: 0,
          durationMs: Math.min(10000, gap),
          navKey: '',
        };
        this.recordedSteps = [...this.recordedSteps, waitStep];
      }
      this.downNx = nx;
      this.downNy = ny;
      this.downTs = now;
      native.injectNormTouch(this.displaySlot, 521, 0, nx, ny);
    } else if (ev.type === TouchType.Move) {
      native.injectNormTouch(this.displaySlot, 521, 1, nx, ny);
    } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
      native.injectNormTouch(this.displaySlot, 521, 2, nx, ny);
      const dur: number = Math.max(40, now - this.downTs);
      const dist: number = Math.hypot(nx - this.downNx, ny - this.downNy);
      const isSwipe: boolean = dist > 0.04;
      const step: MacroStep = {
        id: `st_${now}_${this.recordedSteps.length}`,
        kind: isSwipe ? 'swipe' : 'tap',
        label: isSwipe
          ? `滑动 (${Math.round(this.downNx * 100)}%,${Math.round(this.downNy * 100)}%)→(${Math.round(nx * 100)}%,${Math.round(ny * 100)}%)`
          : `点击 (${Math.round(nx * 100)}%,${Math.round(ny * 100)}%)`,
        nx: this.downNx,
        ny: this.downNy,
        nx2: nx,
        ny2: ny,
        durationMs: dur,
        navKey: '',
      };
      this.recordedSteps = [...this.recordedSteps, step];
      this.lastRecTs = now;
      this.progressText = `已录制 ${this.recordedSteps.length} 步 · ${step.label}`;
    }
  }

  build() {
    Stack({ alignContent: Alignment.TopEnd }) {
      if (this.recording) {
        // Transparent full-viewport touch capture layer forwarding live touches to Android
        Column()
          .width(this.viewW)
          .height(this.viewH)
          .backgroundColor('#14E84026')
          .border({ width: 2, color: '#FFE84026' })
          .onTouch((ev: TouchEvent) => { this.handleRecordTouch(ev); })
      }

      // Docked Compact Automation Panel
      Column({ space: Spacing.SM }) {
        Row({ space: Spacing.SM }) {
          SymbolGlyph($r('sys.symbol.bolt_fill'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.brand')])
          Text('自动化脚本 / 宏')
            .fontSize(FontSize.BODY_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor('#FFFFFF')
            .layoutWeight(1)
          Button('关闭', { type: ButtonType.Capsule })
            .height(24).fontSize(11).backgroundColor('#44FFFFFF').fontColor('#FFFFFF')
            .onClick(() => {
              this.recording = false;
              this.onClose();
            })
        }
        .width('100%')

        // Loop count pills
        Row({ space: 6 }) {
          Text('循环:')
            .fontSize(11)
            .fontColor('#CCFFFFFF')
          ForEach([1, 5, 20, 0], (cnt: number) => {
            Button(cnt === 0 ? '无限' : `${cnt}次`, { type: ButtonType.Capsule })
              .height(24)
              .fontSize(11)
              .fontColor('#FFFFFF')
              .backgroundColor(this.loopCount === cnt ? '#FF0A59F7' : '#33FFFFFF')
              .onClick(() => { this.loopCount = cnt; })
          }, (cnt: number) => String(cnt))
        }
        .width('100%')

        // Recorder controls
        if (this.recording) {
          Row({ space: Spacing.XS }) {
            TextInput({ placeholder: '脚本名称', text: this.draftName })
              .layoutWeight(1)
              .height(28)
              .fontSize(12)
              .fontColor('#FFFFFF')
              .backgroundColor('#33FFFFFF')
              .borderRadius(Radius.SM)
              .onChange((v: string) => { this.draftName = v; })
            Button(`保存(${this.recordedSteps.length})`, { type: ButtonType.Capsule })
              .height(28).fontSize(11).backgroundColor('#FF26B864').fontColor('#FFFFFF')
              .onClick(() => { this.saveRecording(); })
            Button('取消', { type: ButtonType.Capsule })
              .height(28).fontSize(11).backgroundColor('#66FFFFFF').fontColor('#FFFFFF')
              .onClick(() => { this.recording = false; this.recordedSteps = []; })
          }
          .width('100%')
        } else {
          Button('● 开始可视化录制（点/滑屏幕）', { type: ButtonType.Capsule })
            .width('100%')
            .height(30)
            .fontSize(12)
            .fontColor('#FFFFFF')
            .backgroundColor('#FFE84026')
            .onClick(() => { this.startRecording(); })
        }

        if (this.progressText.length > 0) {
          Text(this.progressText)
            .fontSize(11)
            .fontColor('#FF91C7FF')
            .width('100%')
            .maxLines(2)
        }

        // Saved scripts list
        Scroll() {
          Column({ space: 6 }) {
            if (this.scripts.length === 0) {
              Text('暂无已保存脚本，点击上方「开始可视化录制」即可创建')
                .fontSize(11)
                .fontColor('#99FFFFFF')
                .width('100%')
                .padding({ top: 6, bottom: 6 })
            } else {
              ForEach(this.scripts, (sc: MacroScript) => {
                Row({ space: 6 }) {
                  Column({ space: 2 }) {
                    Text(sc.name)
                      .fontSize(12)
                      .fontWeight(FontWeight.Medium)
                      .fontColor('#FFFFFF')
                      .maxLines(1)
                    Text(`${sc.steps.length} 步`)
                      .fontSize(10)
                      .fontColor('#AAFFFFFF')
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Button(this.runningId === sc.id ? '停止' : '运行', { type: ButtonType.Capsule })
                    .height(26)
                    .fontSize(11)
                    .fontColor('#FFFFFF')
                    .backgroundColor(this.runningId === sc.id ? '#FFE84026' : '#FF0A59F7')
                    .onClick(() => { this.toggleRunScript(sc); })

                  Button('删', { type: ButtonType.Capsule })
                    .height(26)
                    .fontSize(11)
                    .fontColor('#FFFFFF')
                    .backgroundColor('#33FFFFFF')
                    .onClick(() => { this.deleteScript(sc.id); })
                }
                .width('100%')
                .padding({ left: 8, right: 8, top: 6, bottom: 6 })
                .borderRadius(Radius.SM)
                .backgroundColor('#22FFFFFF')
              }, (sc: MacroScript) => `${sc.id}_${this.runningId}`)
            }
          }
          .width('100%')
        }
        .constraintSize({ maxHeight: 220 })
        .width('100%')
      }
      .width(Math.min(280, Math.max(220, this.viewW - 24)))
      .padding(Spacing.MD)
      .margin({ top: Spacing.SM, right: Spacing.SM })
      .borderRadius(Radius.CARD)
      .backgroundColor('#E6161B26')
      .border({ width: 0.8, color: '#44FFFFFF' })
      .shadow(ShadowStyle.OUTER_DEFAULT_LG)
    }
    .width(this.viewW)
    .height(this.viewH)
    .hitTestBehavior(this.recording ? HitTestMode.Default : HitTestMode.Transparent)
  }
}
