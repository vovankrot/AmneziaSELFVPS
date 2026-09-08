pragma Singleton

import QtQuick

// ─────────────────────────────────────────────────────────────────────────────
//  Redesign palette — "GitHub-dark + Tailwind" by vovankrot
//  Property NAMES are kept identical to the original theme so the whole app
//  re-skins automatically; only the VALUES are remapped to the new design
//  system:
//      Background #0D1117 · Surface #161B22 · Card #21262D · Border #30363D
//      Primary #3B82F6 · Success #22C55E · Warning #F59E0B · Error #EF4444
//      Font: Inter (see Style/Theme typography)
// ─────────────────────────────────────────────────────────────────────────────
QtObject {
    property QtObject color: QtObject {
        readonly property color transparent: 'transparent'

        // ── Text / light neutrals ────────────────────────────────────────────
        readonly property color paleGray: '#EDF4FA'        // primary text (near white)
        readonly property color lightGray: '#C7D2DC'       // secondary text
        readonly property color mutedGray: '#8291A1'       // muted text
        readonly property color charcoalGray: '#5E6D7B'    // caption / disabled
        readonly property color pearlGray: '#FFFFFF'       // pure white emphasis

        // ── Surfaces (dark) ──────────────────────────────────────────────────
        readonly property color midnightBlack: '#071019'   // app background
        readonly property color darkCharcoal: '#071019'    // background variant
        readonly property color onyxBlack: '#0B1621'       // surface
        readonly property color deepBrown: '#111D29'       // card
        readonly property color benefitsPanelBackground: '#111D29'
        readonly property color slateGray: '#1D2C3B'       // borders / dividers
        readonly property color richBrown: '#243648'       // elevated border

        // ── Accents ──────────────────────────────────────────────────────────
        readonly property color goldenApricot: goldenApricotString  // PRIMARY #2F7DF6
        readonly property color softViolet: '#69A4FF'      // link / light accent
        readonly property color vibrantGreen: '#2DCE76'    // success / connected
        readonly property color vibrantRed: '#EF4444'      // error
        readonly property color burntOrange: '#F59E0B'     // warning
        readonly property color mutedBrown: '#6E7681'      // muted accent

        // ── Home glow / gradient ─────────────────────────────────────────────
        readonly property color accentGradientTop: '#0C1925'
        readonly property color accentGradientMid: '#09131D'
        readonly property color accentGradientBottom: '#050C13'
        readonly property color accentGlowViolet: '#1D64D8'
        readonly property color accentGlowMagenta: '#2F7DF6'
        readonly property color accentGlowOrange: '#2DCE76'   // connected-state glow

        // ── Control tracks ───────────────────────────────────────────────────
        readonly property color accentTrackLavender: '#C9D1D9'
        readonly property color accentTrackShadow: '#30363D'

        // ── Translucent layers ───────────────────────────────────────────────
        readonly property color sheerWhite: Qt.rgba(1, 1, 1, 0.12)
        readonly property color translucentWhite: Qt.rgba(1, 1, 1, 0.08)
        readonly property color barelyTranslucentWhite: Qt.rgba(1, 1, 1, 0.05)
        readonly property color translucentMidnightBlack: Qt.rgba(13/255, 17/255, 23/255, 0.82)
        readonly property color softGoldenApricot: Qt.rgba(47/255, 125/255, 246/255, 0.25)  // primary glow
        readonly property color mistyGray: Qt.rgba(240/255, 246/255, 252/255, 0.82)
        readonly property color cloudyGray: Qt.rgba(240/255, 246/255, 252/255, 0.68)
        readonly property color translucentRichBrown: Qt.rgba(48/255, 54/255, 61/255, 0.34)
        readonly property color translucentSlateGray: Qt.rgba(48/255, 54/255, 61/255, 0.32)
        readonly property color translucentOnyxBlack: Qt.rgba(22/255, 27/255, 34/255, 0.42)

        readonly property string goldenApricotString: '#2F7DF6'
    }
}
