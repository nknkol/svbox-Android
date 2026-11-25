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
    Column({ space: 16 }) {
      Row() {
        Text('重命名 Android 设备')
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('sys.color.font_primary'))
        Blank()
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.xmark'))
            .fontSize(14)
            .fontColor([$r('sys.color.icon_secondary')])
        }
        .width(30)
        .height(30)
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .onClick(() => {
          this.controller.close();
        })
      }
      .width('100%')

      Column({ space: 6 }) {
        TextInput({ placeholder: '请输入新的设备名称', text: this.draftName })
          .width('100%')
          .height(42)
          .fontSize(14)
          .fontColor($r('sys.color.font_primary'))
          .placeholderColor($r('sys.color.font_tertiary'))
          .backgroundColor($r('sys.color.background_secondary'))
          .borderRadius(12)
          .maxLength(40)
          .onChange((val: string) => {
            this.draftName = val;
          })

        Row() {
          Blank()
          Text(`${this.draftName.length} / 40`)
            .fontSize(11)
            .fontColor($r('sys.color.font_tertiary'))
        }
        .width('100%')
      }

      Row({ space: 12 }) {
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

        Button('保存', { type: ButtonType.Capsule })
          .height(36)
          .padding({ left: 22, right: 22 })
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .enabled(this.draftName.trim().length > 0)
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
    .padding(24)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .borderRadius(24)
    .shadow({ radius: 28, color: '#26000000', offsetY: 8 })
  }
}
