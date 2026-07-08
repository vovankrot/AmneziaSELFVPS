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
        readonly property color paleGray: '#F0F6FC'        // primary text (near white)
        readonly property color lightGray: '#C9D1D9'       // secondary text
        readonly property color mutedGray: '#8B949E'       // muted text
        readonly property color charcoalGray: '#6E7681'    // caption / disabled
        readonly property color pearlGray: '#FFFFFF'       // pure white emphasis

        // ── Surfaces (dark) ──────────────────────────────────────────────────
        readonly property color midnightBlack: '#0D1117'   // app background
        readonly property color darkCharcoal: '#0D1117'    // background variant
        readonly property color onyxBlack: '#161B22'       // surface
        readonly property color deepBrown: '#21262D'       // card
        readonly property color benefitsPanelBackground: '#21262D'
        readonly property color slateGray: '#30363D'       // borders / dividers
        readonly property color richBrown: '#30363D'       // elevated border

        // ── Accents ──────────────────────────────────────────────────────────
        readonly property color goldenApricot: goldenApricotString  // PRIMARY #3B82F6
        readonly property color softViolet: '#58A6FF'      // link / light accent
        readonly property color vibrantGreen: '#22C55E'    // success / connected
        readonly property color vibrantRed: '#EF4444'      // error
        readonly property color burntOrange: '#F59E0B'     // warning
        readonly property color mutedBrown: '#6E7681'      // muted accent

        // ── Home glow / gradient ─────────────────────────────────────────────
        readonly property color accentGradientTop: '#161B22'
        readonly property color accentGradientMid: '#0F141A'
        readonly property color accentGradientBottom: '#0D1117'
        readonly property color accentGlowViolet: '#1F6FEB'
        readonly property color accentGlowMagenta: '#3B82F6'
        readonly property color accentGlowOrange: '#22C55E'   // connected-state glow

        // ── Control tracks ───────────────────────────────────────────────────
        readonly property color accentTrackLavender: '#C9D1D9'
        readonly property color accentTrackShadow: '#30363D'

        // ── Translucent layers ───────────────────────────────────────────────
        readonly property color sheerWhite: Qt.rgba(1, 1, 1, 0.12)
        readonly property color translucentWhite: Qt.rgba(1, 1, 1, 0.08)
        readonly property color barelyTranslucentWhite: Qt.rgba(1, 1, 1, 0.05)
        readonly property color translucentMidnightBlack: Qt.rgba(13/255, 17/255, 23/255, 0.82)
        readonly property color softGoldenApricot: Qt.rgba(59/255, 130/255, 246/255, 0.28)  // primary glow
        readonly property color mistyGray: Qt.rgba(240/255, 246/255, 252/255, 0.82)
        readonly property color cloudyGray: Qt.rgba(240/255, 246/255, 252/255, 0.68)
        readonly property color translucentRichBrown: Qt.rgba(48/255, 54/255, 61/255, 0.34)
        readonly property color translucentSlateGray: Qt.rgba(48/255, 54/255, 61/255, 0.32)
        readonly property color translucentOnyxBlack: Qt.rgba(22/255, 27/255, 34/255, 0.42)

        readonly property string goldenApricotString: '#3B82F6'
    }
}
