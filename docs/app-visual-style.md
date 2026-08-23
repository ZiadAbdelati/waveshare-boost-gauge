# BoostGauge app visual-style specification

**Purpose:** restyle the Android Compose dashboard to reproduce the iOS app’s *light, system-native SwiftUI dashboard* appearance—not the current Android dark instrument-console look.

**Ground truth reviewed:** `apps/ios/BoostGauge/BoostGauge/Views/*.swift` and `Support/Helpers.swift`, plus `/tmp/ios_screen.png` and `/tmp/android_screen.png` (reviewed 2026-08-23). Source references below use `path:line`. The two screenshots show different runtime states—iOS is connected/live and Android is disconnected—so state-dependent copy is separated from the visual rules.

## 1. What the iOS screenshot actually shows

### Page and navigation hierarchy

- The screen is a **white, light-mode page** with a generous iOS large navigation title: **“Boost Gauge”** in heavy black, aligned to the leading content edge. The source uses `NavigationView` plus `.navigationTitle("Boost Gauge")`, **not** `NavigationStack` (`Views/StatusView.swift:10,28`). The large-title treatment seen in the screenshot is therefore platform navigation chrome, not a custom text view.
- A standard trailing **refresh** toolbar action (`arrow.clockwise`) sits at the upper right (`StatusView.swift:29-34`). In the screenshot it is rendered with very generous iOS chrome/clear space; do not turn it into an always-visible outlined status control.
- Content is a vertically scrolling, single-column dashboard: `ScrollView > VStack(spacing: 16)` with uniform outer `.padding()` (`StatusView.swift:11-27`). Visually, it has broad side insets, sizable blank space above the title, and 16 pt-like gaps between card groups.
- The app-level destination switcher is a five-item native `TabView`: **Status, Themes, Settings, Calibrate, Logs**, using SF Symbols (`Views/RootView.swift:5-16`). In the screenshot it presents as a floating/raised white rounded tab bar over the bottom content; the selected Status item is blue and contained by a pale selected pill. That exact floating appearance is platform/OS rendering, not custom code in this repository.

### Hero readout card

- The first, dominant group is a full-width **pale system-gray/lavender card**, softly rounded. It is the largest visual object after the title.
- Its main reading is centered: `-12.60` is extremely large, black, bold, rounded-system text with tabular digits; **“psi”** is much smaller, gray, and aligned on the readout’s first-text baseline—not placed on a separate line. Source: `.system(size: 72, weight: .bold, design: .rounded).monospacedDigit()` beside `.title3` (`StatusView.swift:61-67`).
- Beneath it, two compact capsule badges are centered: the zone (**Vacuum**, cyan text on cyan at 18% opacity) and mode (**LIVE**, secondary text on tertiary system fill). The hierarchy is intentionally restrained: these badges support the number rather than compete with it (`StatusView.swift:68-81`).
- A subtle secondary row follows: diagonal up-right SF Symbol + **“Peak 9.5 psi”** in secondary subheadline gray (`StatusView.swift:82-87`).
- A thin, default `Divider()` separates the readout from a 2 × 2 metadata grid (`StatusView.swift:88-97`). Each inset tile is slightly darker than the outer card, with a semibold black value above a small gray label: Theme / Uptime / Firmware / Page. It is a two-column grid with 12 pt gaps, not Android’s two rows of three telemetry fields.

### Sensors, TPMS, footer, and viewport

- **Sensors** is a distinct pale rounded card. The title is a leading black `.headline`; ADS and BMP are small green 8 pt dots with gray semibold captions. Measurements are quiet label/value rows: muted label leading, black tabular number trailing (`StatusView.swift:121-159,241-255`).
- **TPMS** is another distinct pale rounded card. Its heading and optional “low 31.9” secondary caption share a row. Below, four compact, equal mini-cards hold gray wheel abbreviations, bold/tabular black PSI, and a 7 pt green health dot (`StatusView.swift:161-197`).
- At the screenshot crop, TPMS continues under the floating tab bar and the beginning of OBD2 is barely visible. This is normal scroll content, not a fixed dashboard panel.
- The source’s status footer is a small 8 pt red/green dot with monospaced `.caption` transport text, with only 4 pt top spacing (`StatusView.swift:270-289`). It is intentionally quiet and should appear after the cards, not replace the title/readout as the primary visual anchor.

## 2. Token table — iOS style guide

### Color and surface tokens

> **Important precision note:** the inspected iOS source contains **no fixed `#RRGGBB` literals** for this screen. `Color(hex:)` only converts runtime API theme data in the Themes list (`Support/Helpers.swift:71-81`; `ThemesView.swift:60-72`). Therefore no source-authoritative fixed hex exists for the dashboard surfaces or semantic SwiftUI colors. The approximate hex values below are screenshot eyeball checks for this **light screenshot only**; implement semantic platform colors first so Dark Mode/accessibility remain correct.

| Role | Source-ground-truth token | Screenshot check (approx., light mode) | Usage |
|---|---|---:|---|
| Page base | System navigation/scroll background; no custom `.background` | `#FFFFFF` | White page behind cards and title. |
| Elevated/card surface | `Color(.secondarySystemBackground)` | `#F2F1F7` | Hero, Sensors, TPMS, OBD, and placeholder cards (`StatusView.swift:54-56,101-103,295-299`). |
| Inset/tile surface | `Color(.tertiarySystemFill)` | `#E4E3E9` | Hero metadata tiles, TPMS tiles, LIVE pill (`StatusView.swift:79,118,192`). |
| Primary text | `.primary` / default Text | `#000000` | Title, values, headings; adaptive system color. |
| Secondary text | `.secondary` | roughly `#8A8992` | Units, labels, peak, helpers, footer; adaptive system color. |
| Tertiary text/fill | `Color(.tertiarySystemFill)` plus `.secondary` text | roughly `#E4E3E9` / `#8A8992` | Neutral badges and inset groups. |
| Vacuum zone | `.cyan`; badge fill `.cyan.opacity(0.18)` | text roughly `#00B5E2`, fill pale cyan | Vacuum label. **No literal source hex.** |
| Atmosphere / unknown | `.gray` | system gray | Zone label / absent state. |
| Boost / success | `.green` | system green, roughly `#34C759` | Boost zone, good sensor/TPMS/link indicators. **No literal source hex.** |
| Overboost / critical | `.red` | system red, roughly `#FF3B30` | Over zone, faults, invalid readings, failed transport. **No literal source hex.** |
| Warning | `.orange`; `.orange.opacity(0.12)` | system orange, roughly `#FF9500`; pale warning fill | Inline errors and warning banner (`StatusView.swift:257-268`). |
| Selected navigation/custom badge | system tab tint (not explicitly set); `.blue.opacity(0.15)` / `.blue` for Themes custom badge | screenshot selected tab roughly iOS blue | Native selected tab; “custom” badge in Themes (`ThemesView.swift:45-52`). |

**Source/screenshot agreement:** the screenshot’s white base, pale card stack, cyan Vacuum badge, green health dots, black readout, and gray labels agree with the source’s semantic tokens. A hard-coded Android dark palette would disagree. The exact screenshot RGBs are **not** the source contract because UIKit/SwiftUI semantic colors are dynamic.

### Typography tokens

SwiftUI semantic sizes are platform-dynamic; point equivalents below are the normal iOS text-style baseline, while the 72 pt readout is explicit source code.

| Role | Exact SwiftUI declaration | Typical point size / weight | Compose intent |
|---|---|---|---|
| Navigation title | `.navigationTitle(...)` | Native large title, typically 34 pt bold when expanded | Use a 34 sp / `FontWeight.Bold` top title; do not make it a compact app-bar label. |
| Hero PSI number | `.system(size: 72, weight: .bold, design: .rounded).monospacedDigit()` | **72 pt bold**, rounded, tabular | `72.sp`, Bold, rounded/system sans where available; `FontFeatureSettings("tnum")` / tabular figures. |
| Hero unit | `.title3` | typically 20 pt regular | 20 sp regular, secondary. Baseline-align to number. |
| Section title / zone badge | `.headline` | typically 17 pt semibold | 17 sp Semibold. |
| Metric row / peak | `.subheadline` | typically 15 pt regular | 15 sp regular; values tabular. |
| Metadata value | `.subheadline.weight(.semibold)` | typically 15 pt semibold | 15 sp Semibold. |
| Metadata label / TPMS low label | `.caption` | typically 12 pt regular | 12 sp regular. |
| TPMS wheel label | `.caption2` | typically 11 pt regular | 11 sp regular. |
| Status pills | `.caption.weight(.semibold)` | typically 12 pt semibold | 12 sp Semibold. |
| Error/help | `.footnote` | typically 13 pt regular | 13 sp regular. |
| Footer / uptime logs | `.caption.monospaced()` | typically 12 pt monospaced | 12 sp monospaced. |
| Telemetry values | `.monospacedDigit()` | inherits enclosing role | Enable tabular digits, not a full mono font unless the iOS declaration says `.monospaced()`. |

### Geometry, separators, and grouping

| Token | Exact source value | Application |
|---|---:|---|
| Dashboard stack gap | **16** | Between major status cards (`StatusView.swift:12`). |
| Outer scroll inset | `.padding()` | System default all-edge padding (normally 16 pt). |
| Card internal inset | `.padding()` | System default all-edge padding (normally 16 pt). |
| Card corner radius | **16** | All major Status cards. |
| Hero vertical internal gap | **12** | Number, pills, peak, grid. |
| Hero number/unit horizontal gap | **8** | Baseline readout row. |
| Zone/mode gap | **10** | Capsule row. |
| Peak icon/text gap | **6** | Peak row. |
| Hero grid row/column gap | **12** | Two equal columns. |
| Inset tile radius | **10** | Metadata and TPMS tiles. |
| Inset tile vertical padding | **6** metadata; **8** TPMS | Compact but soft tile interiors. |
| Zone capsule padding | horizontal **12**, vertical **4** | Zone badge. |
| Mode capsule padding | horizontal **10**, vertical **4** | LIVE/DEMO badge. |
| Error banner | padding **10**, radius **10**, orange at **12%** opacity | Small inline banner; no huge red paragraph. |
| Presence dot | **8 × 8** (TPMS: **7 × 7**) | Health indicator. |
| Divider | Default `Divider()` | One light, thin separator inside the hero; no heavy rules. |

### Controls, lists, and state presentation

| Item | iOS rule | Compose equivalent look |
|---|---|---|
| Refresh | Native toolbar `Button` with `arrow.clockwise` SF Symbol | Top-end `IconButton`, no outlined connection chip. |
| Tabs | Native five-item `TabView` with SF Symbols | Five-item NavigationBar visually lifted as a white rounded surface; selected item gets pale container + blue icon/label. |
| Settings / Calibration | Native `Form` with titled `Section`s | `LazyColumn` of clearly separated, inset-grouped settings sections; use Material controls but avoid bespoke dark cards. |
| Themes / populated Logs | Native `List`, system separators/insets | `LazyColumn` rows with subtle dividers and grouped surface; avoid chunky card-per-row. |
| Picker | Native segmented picker for Transport; default picker otherwise | `SingleChoiceSegmentedButtonRow` for transport; dropdown/dialog or exposed selector for other pickers. |
| Toggle / Stepper / text field | Native SwiftUI controls, no custom fill | Material 3 controls using iOS-light surface/text tokens; retain standard touch targets. |
| Loading | `ProgressView` overlay or inline beside action label; disabled action while active | Center/inline `CircularProgressIndicator`; do not replace the dashboard with a neon numeric placeholder. |
| Empty | Centered light stack: secondary tray icon (`.largeTitle`), secondary text, optional orange footnote, normal button | Centered low-emphasis empty state on page background. |
| Error / success | Inline `.footnote`: orange error, green confirmation. Status screen additionally has a compact orange 12%-fill banner. | One compact assistive message/banner; never make a transport error the primary visual object. |

## 3. Android gap analysis

### What the Android screenshot shows

The Android dashboard currently uses a nearly black base, dark gray cards, and a dark system/navigation bar. The top of the page is occupied by an outlined **Disconnected** chip with a red dot, a lime refresh icon, then a large multi-line pink/red HTTP failure string. The hero is a charcoal-purple card with lime `0.00`, a separate gray `psi` line, and cyan peak text. A six-field metadata card follows in a 3-column × 2-row layout. TPMS and OBD are dark cards with broadly spaced placeholder columns. A full-width dark bottom navigation bar spans the screen above the Android system navigation area; its selection is lavender.

### Differences to fix

1. **Theme:** iOS is light white + adaptive pale system surfaces. Android is a high-contrast dark technical console. Switch the default/restyled dashboard to light surfaces.
2. **Top hierarchy:** iOS gives the title and PSI readout priority. Android gives a failed connection and raw network exception priority. Keep disconnection visible but demote it to the compact status footer/inline banner pattern.
3. **App title:** Android has no large “Boost Gauge” anchor. Add a leading 34 sp bold title with breathing room and a top-end refresh action.
4. **Readout color and arrangement:** iOS reads black, 72 pt, rounded, tabular, with secondary `psi` baseline-aligned. Android uses lime, display-style numerals and puts the unit below. Change both the color and the baseline layout.
5. **Hero anatomy:** iOS has zone + LIVE capsules, a muted peak row, a thin divider, then a **2 × 2** inset tile grid. Android omits the iOS badge arrangement and uses a separate dense 3 × 2 metadata card.
6. **Card surfaces:** iOS uses one pale elevated surface with slightly darker inset tiles. Android creates several nearly-black card layers and a purple hero. Replace charcoal/lavender surfaces with `secondarySystemBackground`-like / `tertiarySystemFill`-like layers.
7. **Sensor treatment:** iOS uses a clean leading-label/trailing-value list with tiny health dots. Android’s shown screen lacks an equivalent Sensors card, instead exposing diagnostic density in metadata/OBD.
8. **TPMS:** iOS nests four compact rounded mini-cards inside the TPMS group, each with label, tabular PSI, health dot. Android uses unbounded wide columns with dashes and no mini-surface hierarchy.
9. **Type hierarchy:** Android labels and values use similar pale/lavender weights across cards. iOS sharply separates black headings/values, medium-gray labels, and smaller helper text.
10. **Accent semantics:** iOS color is sparse and semantic: cyan only for Vacuum, green for healthy, red only for invalid/critical, blue for selected navigation. Android adds lime primary telemetry, cyan peak, lavender selection, and prominent red error simultaneously.
11. **Navigation:** Android’s rectangular full-width dark bar is visually heavier than the content. Match the iOS visual effect with a light raised/rounded navigation surface, blue selected state, and a pale selected-item container. Compose should remain Android-accessible; it need not literally float over gesture navigation.
12. **Runtime-state caveat:** the iOS sample is live and Android is disconnected. Preserve Android’s failure state, but render it as a compact orange/red supporting element; do not falsely show LIVE or live values.

## 4. Concrete Android / Compose translation table

### ColorScheme and shapes

| iOS token | Compose Material 3 mapping | Recommended implementation |
|---|---|---|
| White page base | `colorScheme.background` | `Color.White` for the supplied light reference; use this behind the scroll list. |
| `secondarySystemBackground` card | `colorScheme.surfaceContainerLow` (or a named `BoostCard`) | Light reference target: approximately `#F2F1F7`; use for hero/section cards. |
| `tertiarySystemFill` inset | `colorScheme.surfaceContainerHighest` (or `BoostInset`) | Light reference target: approximately `#E4E3E9`; use for mini-cards, neutral pills. |
| Primary text | `colorScheme.onBackground` / `onSurface` | Near black in this light reference. |
| Secondary text | `colorScheme.onSurfaceVariant` | Gray; use for labels, units, help, peak, footer. |
| Vacuum cyan | named `BoostVacuum` / `tertiary` only if unambiguous | Use a cyan matching platform/system cyan; zone background = `copy(alpha = 0.18f)`. |
| Healthy green | `colorScheme.primary` **only if** primary is reserved for health, otherwise named `BoostSuccess` | Keep semantic green dots and boost/link state. |
| Critical red | `colorScheme.error` | Invalid/fault/disconnected dot. |
| Warning orange | named `BoostWarning` | Use `BoostWarning.copy(alpha = .12f)` for a compact warning container. |
| Navigation blue | `colorScheme.primary` | Choose iOS-like blue for selected tab icon/text; selected container = `primary.copy(alpha ≈ .10–.15f)`. |
| Major cards | `Shapes.large` | `RoundedCornerShape(16.dp)`. |
| Inset tiles / warning banner | `Shapes.medium` | `RoundedCornerShape(10.dp)`. |
| Pills | `Shapes.extraLarge` or `CircleShape` | Fully rounded Capsule equivalent. |

### Typography and layout recipe

| iOS token / structure | Compose implementation |
|---|---|
| Large nav title | Put `Text("Boost Gauge")` above the scroll content or in an expanded top area: `fontSize = 34.sp`, `fontWeight = FontWeight.Bold`, `color = onBackground`; horizontal 16 dp content inset. This is the Compose-idiomatic equivalent of iOS’s expanded NavigationView title. |
| Refresh toolbar action | `IconButton` aligned end in the same header row, using `Icons.Outlined.Refresh`; transparent background, normal Material touch target. |
| Scroll dashboard | `LazyColumn(contentPadding = PaddingValues(horizontal = 16.dp, vertical = 16.dp), verticalArrangement = Arrangement.spacedBy(16.dp))`. Add bottom content padding sufficient for the navigation bar. |
| Hero card | `Surface(shape = RoundedCornerShape(16.dp), color = BoostCard)` + `Column(verticalArrangement = Arrangement.spacedBy(12.dp), horizontalAlignment = Alignment.CenterHorizontally)` + `padding(16.dp)`. |
| PSI row | `Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp))`: `Text` at **72.sp / Bold**, tabular figures and rounded/system sans; `psi` at **20.sp**, secondary. Do not place `psi` underneath. |
| Zone/mode badges | Centered Row with 10 dp gap. Zone `Text` at 17 sp Semibold with `PaddingValues(horizontal=12.dp, vertical=4.dp)`, cyan 18%-alpha container; mode uses 12 sp Semibold + 10 × 4 dp neutral pill. |
| Peak | Row with 6 dp gap, 15 sp, `onSurfaceVariant`; use northeast/up-right icon. |
| Divider | `HorizontalDivider(color = onSurface.copy(alpha = 0.18f))` once between peak and grid. |
| Metadata grid | Two columns only: `Row(horizontalArrangement = spacedBy(12.dp))` twice or `LazyVerticalGrid(GridCells.Fixed(2))`; each inset uses 10 dp radius, vertical 6 dp padding, 15 sp semibold value over 12 sp gray label. |
| Sensor metrics | 15 sp row, label `onSurfaceVariant`, trailing value `onSurface` with `FontFeatureSettings("tnum")`; group title 17 sp Semibold; 8 dp intra-card spacing. |
| TPMS tile grid | Four equal weighted inset surfaces, 8 dp gap, vertical 8 dp padding, 11 sp wheel label, 17 sp semibold/tabular value, 7 dp health dot. |
| Status footer | 8 dp dot + 12 sp monospaced-like/medium label; 4 dp top gap. It should follow scroll content rather than sit in a dominant top chip. |
| Bottom destinations | `NavigationBar(containerColor = Color.White)` with five items. To approximate iOS, use a low-elevation rounded parent surface and a lightly tinted selected-item indicator. Keep adequate bottom inset for Android navigation/gestures. |

### Compose Typography object sketch

Use these roles in a local `Typography`/named style layer rather than scattering raw values:

```kotlin
val BoostHeroValue = TextStyle(
    fontSize = 72.sp,
    lineHeight = 78.sp,
    fontWeight = FontWeight.Bold,
    fontFeatureSettings = "tnum"
)
val BoostNavTitle = TextStyle(fontSize = 34.sp, fontWeight = FontWeight.Bold)
val BoostSectionTitle = TextStyle(fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
val BoostMetric = TextStyle(fontSize = 15.sp)
val BoostMetricEmphasis = TextStyle(fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
val BoostCaption = TextStyle(fontSize = 12.sp)
val BoostCaption2 = TextStyle(fontSize = 11.sp)
val BoostFootnote = TextStyle(fontSize = 13.sp)
```

`fontFeatureSettings = "tnum"` is the Compose counterpart for SwiftUI `.monospacedDigit()`. Do **not** apply a full monospaced face to normal labels; only uptime/footer text uses SwiftUI `.monospaced()` in the source.

### iOS-only conventions and Compose-idiomatic equivalents

| iOS convention | Compose equivalent that preserves the look |
|---|---|
| `NavigationView` automatic expanded large title | A scroll-aware/expanded header or a simple large title inside the top content; avoid relying on a compact `TopAppBar` title alone. |
| Native `TabView` shown as a floating white capsule in the screenshot | Material 3 `NavigationBar` inside a rounded elevated `Surface`; preserve labels, blue selected content, and pale selected indicator rather than copying iOS safe-area behavior literally. |
| `Form` and `List` automatic inset grouping/separators | `LazyColumn` sections/rows with 16 dp outer insets, light grouped surfaces where needed, subtle dividers, Material controls. |
| Dynamic `Color(.secondarySystemBackground)` / system semantic colors | Named Compose light/dark semantic tokens. Do not freeze screenshot RGBs as the only palette. |
| SF Symbols | Material Icons or matching vector assets with similar optical weight; use existing Android symbols where semantically equivalent. |

## Acceptance checklist for the Android restyle

- [ ] Default dashboard is white/light, with pale `#F2F1F7`-like cards and darker `#E4E3E9`-like inset tiles—not black/charcoal.
- [ ] “Boost Gauge” is a leading large title; refresh is a quiet top-end icon action.
- [ ] PSI is black, 72 sp-ish, bold/rounded/tabular; `psi` is secondary and baseline-aligned.
- [ ] Hero order is reading → zone/LIVE badges → peak → divider → 2 × 2 metadata tiles.
- [ ] Sensor and TPMS cards use the iOS label/value and compact-tile hierarchy.
- [ ] Red/orange disconnected/error UI is compact and secondary; raw HTTP exception strings are not the screen’s visual headline.
- [ ] Five destinations remain labelled, but the navigation surface is light with blue selected state and a pale selection container.
- [ ] Settings, Themes, Calibration, and Logs use light grouped/list conventions and native-feeling Material controls, mirroring their `Form`/`List` SwiftUI source.
