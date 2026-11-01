# Web UI Visual Refresh

## Goal

Give the on-device Web UI a modern, BoardUI-style look (clean cards, clear hierarchy, consistent spacing and controls, light and dark themes) without changing behavior, the HTTP API, or the no-build, no-framework asset model.

## Fixed constraints

- **Assets stay three hand-written files** (`index.html`, `app.css`, `app.js`) embedded in flash. No framework, no npm build step, no web fonts, no CDN: the CSP allows only `'self'` for scripts, styles, and connections, and the UI must work on a LAN without Internet.
- **Size budgets** (`host_tests/test_web_assets.mjs`): `app.css` and `index.html` < 20,000 bytes each, `app.js` < 64 KiB. Current sizes: `app.css` 19,066, `index.html` 17,708, `app.js` 61,954. The refresh is mostly CSS, so the CSS budget is the real constraint (see Decisions).
- **JavaScript hooks are frozen**: every `id`, `data-*` attribute, and class that `app.js` reads or writes stays, including `active`, `is-stale`, `rule-triggered`, `notice`/`error`/`success`, `state`/`state-ok|warn|bad`, `value-ok|warn|bad`, `subtle`, `item`, `item-title`, `item-meta`, `item-actions`, `recent-actions`, `recent-name`, `empty`, `primary`, `danger`, `security-note`. `setStatus` replaces `className` wholesale, so tone styling must keep coming only from those classes.
- **Breakpoints stay** at 820px, 560px (also hard-coded in `app.js`), and 320px.
- **Test-pinned CSS** (system runtime grid, mobile recent-actions grid, rule-trigger pulse, reduced motion, firmware disclosure summary) may change only together with a deliberate update of the matching assertion.
- No inline `style` attributes or `<style>` blocks (CSP `style-src 'self'`).

## Design direction

- **Tokens first.** Rebuild `:root` as a compact token set: neutral slate scale, one teal accent (keeps the current brand), semantic ok/warn/bad/info with background and border variants, radius scale (6/10/14px), spacing on a 4px grid, two shadow levels, and a type scale on the system font stack with tabular numerals for codes and counters.
- **Themes.** Dark stays the default look; add a light theme through `prefers-color-scheme: light` by overriding tokens only, and set `<meta name="color-scheme" content="dark light">`. Every component reads tokens, so there is one stylesheet with two token blocks.
- **Surfaces.** Replace gradient buttons and stacked bands with flat cards: 1px border, subtle shadow, consistent 16–20px padding, section headers with title, description, and right-aligned actions.
- **Controls.** One button system (primary solid accent, secondary, ghost/quiet, danger) at 40px (44px on mobile); filled inputs with a visible focus ring; pill badges with a status dot; segmented-style tabs.
- **Data display.** Monospace code chips for RF codes and fingerprints, key/value lists with dividers in System, and an emphasized "latest frame" card on Overview.
- **Accessibility.** WCAG AA contrast in both themes, visible focus on every control, reduced-motion support kept, no information conveyed by color alone (status dots keep their text labels).

## Phases

### 0. Validation harness (done)

Commit a deterministic mock server (`tools/web-mock.mjs`, Node, no dependencies) that serves the real assets and a scripted `/api/*`, intercepts every mutation without side effects, and has scenario switches: healthy, disconnected, list endpoint failure, OTA reboot, Generic GPIO reboot, long names and full lists, empty lists. Register a smoke test so it can't drift from the API shape. Capture baseline screenshots of all five views at 1280, 768, and 375px.

Result: `tools/web-mock.mjs` with scenarios `healthy`, `empty`, `busy`, `degraded`, `list-failure`, `disconnected`, and `rebooting`; `host_tests/test_web_mock.mjs` requires the mock's JSON fields to match the firmware handlers exactly; `tools/web-screenshots.mjs` captures full-page screenshots with headless Chromium. Baselines for `healthy`, `empty`, `busy`, and `degraded` (dark, 1280/768/375) are in the ignored `.local/web-screenshots/baseline/`; all rendered without page errors or horizontal overflow.

### 1. Tokens, base, and controls (done)

Rewrite `:root`, typography, buttons, inputs, selects, toggles, badges, notices, and focus states. Remove duplicated rules while rewriting, so the stylesheet gets smaller as it changes.

Result: a dark token set that meets WCAG AA (text at least 5.5:1, primary button label 7.5:1, field borders at least 3.9:1) with tokens for hover, tint, primary, and semantic ink colors, replacing the hard-coded colors outside the header. Buttons are flat (solid teal primary with dark label, raised secondary, tinted danger, ghost quiet); fields are filled with an accent focus border; labels and section titles are larger and use the main text color. `app.css` is 19,599 bytes, so phases 2 and 3 must remove more than they add. Follow-ups for phase 3: give section headings breathing room above their cards, and make per-row Replay buttons secondary so long lists aren't a column of solid teal.

### 2. App shell (done)

Header (brand, device name, connection badge), tab navigation (pill/segmented on desktop, full-width icon tabs on mobile), and notices shown as an unobtrusive toast-style banner that doesn't shift the layout. Same DOM; class additions only where needed.

Result: CSS only, no HTML or JavaScript changes. On desktop the header and tab bar share one translucent sticky surface, and the 60px header and pill tabs replace the 68px header and underline tabs. Mobile keeps the full-width icon tabs with an accent underline. The brand mark now uses tokens, leaving no hard-coded colors outside the token block. Notices are a fixed toast (bottom right on desktop, full width on mobile) with a `--shadow-md` token; `main` reserves bottom padding while a notice is visible so a persistent error can't hide the last controls. `app.css` is 19,881 bytes: phase 3 must shrink the view styles, and the phase 4 light theme will need the budget decision.

### 3. Views, one at a time

- **Overview:** status cards with a colored indicator, a prominent latest-frame card, a compact learning card, and a denser activity table.
- **Signals:** recent and learned lists as rows with a code chip, metadata line, and grouped actions; the save form as a card with labelled field groups.
- **Transmit:** decoded and raw forms in cards with helper text and a clear primary action.
- **Rules:** "trigger → target" rows with chips, automation settings as a settings card.
- **System:** health summary cards, key/value groups, styled disclosures, and the firmware card with its security note.

Each view lands with before/after screenshots at all three widths.

Result (done, CSS only): status cards are flat with a status dot instead of a colored top bar and show larger values; the latest frame is a card with two label/value pairs per row on desktop (one per row at 820px and below) and monospace values; list-row Replay buttons are secondary so only primary actions are solid teal; System groups separate key/value rows with dividers; section titles placed directly in a band (Transmit) get proper spacing. Eight redundant `letter-spacing: 0` declarations, three duplicate `.metric strong.value-*` rules, and the `.first-band` rule were removed, and tabular figures are set once globally. `app.css` is 19,904 bytes. Code chips for RF codes were not done: they need markup that `app.js` generates, so they would cost JavaScript changes.

### 4. Light theme and contrast pass (done)

Add the light token block, then check contrast for text, borders, badges, and focus rings in both themes.

Result: a `prefers-color-scheme: light` block overrides only tokens, and `index.html` declares `color-scheme: dark light`. Every text pair meets WCAG AA (body text 16.6:1, tertiary text at least 4.9:1, white on the primary button 5.1:1, status colors on their tinted backgrounds at least 5.2:1); field borders are at least 3.3:1. The accent was darkened to `#0a7a84` so icons and focus rings keep margin on the page background. Screenshots of `healthy`, `busy`, `degraded`, `empty`, and `list-failure` in light, plus a dark regression pass, show no page errors or horizontal overflow. Decision 1 was applied: the theme took `app.css` to 20,899 bytes, so its limit is now 24,000 bytes (`index.html` stays at 20,000).

### 5. Verification and release

Result (done except release): host tests pass (12/12). Against the mock in the built-in browser: SSE live updates added a reception immediately; a simulated outage showed Disconnected with the System view marked stale, then recovered to Connected over SSE without a reload; the Generic GPIO flow went Rebooting, reconnected, and confirmed the new TX GPIO with a success toast; an intercepted OTA upload showed an error toast and restored the controls. Both mutations carried the page's own Origin and the exact content types the firmware validates (`application/x-www-form-urlencoded`, `application/octet-stream`); the mock now records these headers. The console showed only the expected network errors from the simulated outages and the intercepted upload. Before/after comparison images are in the ignored `.local/web-screenshots/`. CI run 41342747552 passed the four-profile production verifier at `a9b3a9f`, which contains every firmware-affecting change of the refresh. Not done: on-device check (needs hardware approval) and a release.

- Host tests, with deliberately updated CSS assertions.
- Browser validation on the mock per `AGENTS.md`: desktop/tablet/mobile, both color schemes, horizontal overflow, console errors, polling and SSE fallback, same-origin mutation headers, OTA and Generic GPIO reboot recovery, and visual review of full-page screenshots against the baseline.
- `tools/verify-production.sh` once on the final candidate.
- On-device check only with explicit approval of the board and port.

## Decisions

1. **CSS budget.** Resolved in phase 4: `app.css` limit raised to 24,000 bytes. Recommended: rewrite compactly and keep the 20,000-byte limit if possible; if the light theme pushes past it, raise the `app.css` limit to 24,000 bytes in one deliberate change. Alternative: serve gzip-precompressed assets (changes `web_api.cpp` and the build) and budget on compressed size.
2. **Theme selection.** Applied: follows the system setting. Recommended: follow the system setting only (CSS-only, no JavaScript bytes). Alternative: a manual toggle stored in `localStorage` (about 400 bytes of JavaScript).
3. **Mobile navigation.** Applied: top tab bar kept. Recommended: keep the top tab bar, restyled. Alternative: a fixed bottom tab bar (better thumb reach, needs safe-area handling and more layout testing).

## Out of scope

JavaScript frameworks, behavior or API changes, new features, icon font or web font downloads, and the gzip asset pipeline unless chosen in Decision 1.

## Risks

- **CSS budget pressure** from the second theme; mitigated by token-only overrides and removing duplicated rules.
- **Silent hook breakage** if a class used by `app.js` is renamed; mitigated by the frozen-hook list above and mock-based checks of every status state.
- **Layout regressions** on narrow phones and long names; covered by the mock's long-name scenario and the 320/375px checks.
