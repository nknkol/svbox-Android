import { curves } from '@kit.ArkUI';

/**
 * Unified HarmonyOS PC Design Tokens
 * Strictly aligned with Huawei HarmonyOS 8vp/4vp grid, corner radius levels,
 * icon scale, typography scale, and official spring/bezier motion curves.
 */
export class Spacing {
  static readonly XS: number = 4;    // Micro gap: icon-to-badge, tiny inline gaps
  static readonly SM: number = 8;    // Tight gap: chips, toolbar icon groups, inner small padding
  static readonly MD: number = 12;   // Standard gap: list items, card sections, button groups
  static readonly LG: number = 16;   // Card inner padding, group spacing
  static readonly XL: number = 20;   // Page / workspace horizontal padding
  static readonly XXL: number = 24;  // Sheet & dialog primary padding
}

export class IconSize {
  static readonly XS: number = 12;   // Badges, dropdown chevrons, micro chips
  static readonly SM: number = 16;   // Toolbar icons, settings nav icons, emulator top bar icons
  static readonly MD: number = 20;   // Left navigation rail icons, primary circular action icons
  static readonly LG: number = 24;   // Device preview thumbnail icons, prominent icons
  static readonly XL: number = 44;   // Empty state illustrations
}

export class FontSize {
  static readonly TITLE_LG: number = 18;
  static readonly TITLE_MD: number = 16;
  static readonly TITLE_SM: number = 15;
  static readonly BODY_LG: number = 14;
  static readonly BODY_MD: number = 13;
  static readonly CAPTION: number = 12;
  static readonly MICRO: number = 11;
  static readonly BADGE: number = 10;
}

export class Radius {
  static readonly BADGE: number = 6;
  static readonly SM: number = 8;
  static readonly MD: number = 12;
  static readonly CARD: number = 16;
  static readonly CAPSULE: number = 18;
  static readonly SHEET: number = 24;
}

export class ControlSize {
  static readonly TITLE_BAR_H: number = 48;
  static readonly EMU_BAR_H: number = 40;
  static readonly FOOTER_BAR_H: number = 32;
  static readonly NAV_RAIL_W: number = 76;
  static readonly BTN_LG: number = 40;
  static readonly BTN_MD: number = 36;
  static readonly BTN_SM: number = 32;
  static readonly BTN_XS: number = 24;
  static readonly INPUT_H: number = 36;
}

export class Motion {
  // Huawei HarmonyOS standard curves
  static readonly STD_CURVE: ICurve = curves.cubicBezierCurve(0.4, 0.0, 0.2, 1.0);
  static readonly DECEL_CURVE: ICurve = curves.cubicBezierCurve(0.0, 0.0, 0.2, 1.0);
  static readonly ACCEL_CURVE: ICurve = curves.cubicBezierCurve(0.4, 0.0, 1.0, 1.0);
  // Huawei physical spring curves (velocity, mass, stiffness, damping)
  static readonly SPRING_SMOOTH: ICurve = curves.springCurve(0, 1, 228, 30);
  static readonly SPRING_SNAPPY: ICurve = curves.springCurve(0, 1, 320, 34);

  static readonly DUR_MICRO: number = 150;
  static readonly DUR_SHORT: number = 200;
  static readonly DUR_MEDIUM: number = 260;

  static pageSwitchTransition(): TransitionEffect {
    return TransitionEffect.asymmetric(
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ y: 12 }))
        .animation({ duration: Motion.DUR_MEDIUM, curve: Motion.DECEL_CURVE }),
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ y: -8 }))
        .animation({ duration: Motion.DUR_MICRO, curve: Motion.ACCEL_CURVE })
    );
  }

  static tabContentTransition(): TransitionEffect {
    return TransitionEffect.asymmetric(
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ x: 10 }))
        .animation({ duration: Motion.DUR_SHORT, curve: Motion.DECEL_CURVE }),
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ x: -6 }))
        .animation({ duration: 120, curve: Motion.ACCEL_CURVE })
    );
  }

  static toolbarSwapTransition(): TransitionEffect {
    return TransitionEffect.asymmetric(
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ y: -6 }))
        .combine(TransitionEffect.scale({ x: 0.99, y: 0.99 }))
        .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY }),
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ y: 6 }))
        .animation({ duration: 140, curve: Motion.ACCEL_CURVE })
    );
  }

  static cardTransition(): TransitionEffect {
    return TransitionEffect.asymmetric(
      TransitionEffect.OPACITY
        .combine(TransitionEffect.translate({ y: 14 }))
        .combine(TransitionEffect.scale({ x: 0.98, y: 0.98 }))
        .animation({ duration: Motion.DUR_MEDIUM, curve: Motion.SPRING_SMOOTH }),
      TransitionEffect.OPACITY
        .combine(TransitionEffect.scale({ x: 0.96, y: 0.96 }))
        .animation({ duration: Motion.DUR_SHORT, curve: Motion.ACCEL_CURVE })
    );
  }

  static pillPopTransition(): TransitionEffect {
    return TransitionEffect.OPACITY
      .combine(TransitionEffect.scale({ x: 0.92, y: 0.92 }))
      .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY });
  }

  static checkboxSlideTransition(): TransitionEffect {
    return TransitionEffect.OPACITY
      .combine(TransitionEffect.translate({ x: -12 }))
      .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY });
  }
}
