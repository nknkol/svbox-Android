import {
  ControlSize,
  FontSize,
  IconSize,
  Radius,
  Spacing
} from '../common/DesignTokens';

@CustomDialog
export struct RenameDialog {
  controller: CustomDialogController;
  initialName: string = '';
  onConfirm: (newName: string) => void = (): void => {};

  @State private draftName: string = '';

  aboutToAppear(): void {
    this.draftName = this.initialName;
  }

  build() {
    Column({ space: Spacing.LG }) {
      Row() {
        Text('重命名 Android 设备')
          .fontSize(FontSize.TITLE_LG)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('sys.color.font_primary'))
        Blank()
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.xmark'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.icon_secondary')])
        }
        .width(ControlSize.BTN_SM)
        .height(ControlSize.BTN_SM)
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .hoverEffect(HoverEffect.Highlight)
        .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
        .onClick(() => {
          this.controller.close();
        })
      }
      .width('100%')

      Column({ space: Spacing.SM }) {
        TextInput({ placeholder: '请输入新的设备名称', text: this.draftName })
          .width('100%')
          .height(ControlSize.BTN_LG)
          .fontSize(FontSize.BODY_LG)
          .fontColor($r('sys.color.font_primary'))
          .placeholderColor($r('sys.color.font_tertiary'))
          .backgroundColor($r('sys.color.background_secondary'))
          .borderRadius(Radius.MD)
          .maxLength(40)
          .onChange((val: string) => {
            this.draftName = val;
          })

        Row() {
          Blank()
          Text(`${this.draftName.length} / 40`)
            .fontSize(FontSize.MICRO)
            .fontColor($r('sys.color.font_tertiary'))
        }
        .width('100%')
      }

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
            this.controller.close();
          })

        Button('保存', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.XXL, right: Spacing.XXL })
          .fontSize(FontSize.BODY_LG)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .enabled(this.draftName.trim().length > 0)
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
          .onClick(() => {
            const trimmed: string = this.draftName.trim();
            if (trimmed.length > 0) {
              this.onConfirm(trimmed);
              this.controller.close();
            }
          })
      }
      .width('100%')
    }
    .width(420)
    .padding(Spacing.XXL)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .borderRadius(Radius.SHEET)
    .shadow(ShadowStyle.OUTER_DEFAULT_LG)
  }
}
