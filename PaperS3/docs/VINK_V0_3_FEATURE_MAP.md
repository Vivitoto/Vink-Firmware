# Vink v0.3 Feature Map: ReadPaper V1.7.6 + Legado

Baseline references:

- ReadPaper: `shinemoon/M5ReadPaper` remote `main`, commit `e910d29`, `data/version = V1.7.6`.
- Legado: `gedoor/legado` remote `master`, local reference `/home/vito/.openclaw/workspace/Vink/reference-firmware/legado`.

## Principle

ReadPaper's features should be reused, but not its product UI structure. Vink keeps four primary tabs:

1. Reader
2. Library
3. Transfer / Sync
4. Settings

ReadPaper's click model is useful:

```text
touch event -> state machine -> current state handler -> semantic action -> render/commit
```

Vink should preserve that model, but centralize Vink-specific hitboxes in `VinkUiRenderer::hitTest()` rather than copying ReadPaper's grid coordinates.

## Tab Mapping

### 1. Reader tab

Purpose: current reading session and in-book actions.

ReadPaper features that fit here:

- Current page rendering.
- Previous/next page.
- Center tap opens reader menu.
- Auto-read / auto page turning.
- Manual bookmark/tag for current page.
- TOC / chapter list.
- Bookmark/tag list.
- Jump to page.
- Reading statistics.
- Current-page screenshot action.
- Quick reader menu actions:
  - refresh/current page
  - font scale quick adjustment
  - display label toggle
  - underline/draw-bottom toggle
  - vertical text toggle
  - keep original simplified/traditional setting for current book

Vink UI shape:

- Main card: current book / progress / resume button.
- Secondary cards: TOC, bookmarks, stats, reader tools.
- Reader menu should be a Vink overlay or subpage, not ReadPaper's original menu panel.

Implementation notes:

- State handlers: `handleReader()`, `handleReaderMenu()`, `handleToc()`, `handleBookmarks()`, `handleGotoPage()`.
- Use ReadPaper's `handleReadingState()` behavior as logic reference.
- Do not use direct `M5.Display` writes from reader UI; render into global canvas and commit through `DisplayService`.

### 2. Library tab

Purpose: books, directories, recent/history, local and remote book list.

ReadPaper features that fit here:

- Main menu book list.
- SD directory navigation.
- Open book.
- Recent/history list.
- Toggle recent/file-name mode.
- Refresh/rescan book cache.
- Reindex current/new book.
- Clean orphan bookmarks.
- Book source grouping if needed later.

Legado features that fit here:

- `GET /getBookshelf` from Legado Web service.
- Optional online book listing cached as a remote bookshelf.
- `GET /getChapterList?url=...` for remote book TOC.
- `GET /getBookContent?url=...&index=...` if Vink reads remote content directly.

Vink UI shape:

- Local shelf grid/list.
- Recent row.
- Remote/Legado shelf section if enabled.
- Import/rescan actions.

Implementation notes:

- State handlers: `handleLibrary()`, `handleBookList()`, `handleRemoteShelf()`.
- ReadPaper's `state_main_menu.cpp` can guide selection, page navigation, directory navigation, and open-book flow.
- Vink should avoid ReadPaper's dense 10-row side command grid; use cards and contextual actions.

### 3. Transfer / Sync tab

Purpose: external connectivity and data movement.

ReadPaper features that fit here:

- WiFi hotspot / web UI.
- WebDAV config and WebDAV pull/download.
- USB MSC mode.
- Screenshot/export access.
- Wire/wireless connect mode selection.

Legado features that fit here:

Legado Web service must be enabled in the Android app first.

HTTP endpoints from `api.md` and `HttpServer.kt`:

- `GET /getBookshelf`
- `GET /getChapterList?url=xxx`
- `GET /getBookContent?url=xxx&index=1`
- `POST /saveBookProgress`
- `POST /saveBook`
- `POST /deleteBook`
- `GET /cover?path=xxxxx`
- `GET /image?url=${bookUrl}&path=${picUrl}&width=${width}`

Progress payload uses `BookProgress.kt`:

```json
{
  "name": "book name",
  "author": "author",
  "durChapterIndex": 0,
  "durChapterPos": 0,
  "durChapterTime": 1710000000000,
  "durChapterTitle": "chapter title"
}
```

Vink UI shape:

- Legado connection card: host/port/status.
- Sync now card: pull shelf / push progress / pull progress.
- WiFi transfer card.
- USB mode card.
- WebDAV card.
- Export/screenshots card.

Implementation notes:

- State handlers: `handleTransfer()`, `handleLegadoSync()`, `handleWifiUpload()`, `handleUsbMsc()`, `handleWebDav()`.
- USB MSC should be a modal/confirm state, not a casual toggle. It needs SD access quiescence like ReadPaper.
- Legado is not WebDAV. Treat it as Android app HTTP service on LAN, usually port `1234` for HTTP and `1235` for websocket debug/search.

### 4. Settings tab

Purpose: persistent configuration.

ReadPaper features that fit here:

- Display mode / refresh strategy.
- Dark mode.
- Fast refresh toggle.
- Rotation / auto-rotation.
- Font selection.
- Font scale percentage.
- Font render tradeoff / quality.
- Horizontal/vertical text default.
- Simplified/traditional conversion mode.
- Label position / mark theme / page style.
- Auto-read speed.
- Main menu file count.
- Sleep/shutdown timers.
- WiFi credentials.
- WebDAV credentials.
- Legado endpoint settings.
- About/version/debug info.

Vink UI shape:

- Display card.
- Reading card.
- Network card.
- System card.

Implementation notes:

- State handlers: `handleSettings()`, plus subpages for display/reading/network/system.
- Config should be Vink-owned, but can copy ReadPaper's `GlobalConfig` fields where practical.

## Things That Do Not Fit Cleanly

### USB MSC

Problem: USB MSC can conflict with normal SD filesystem access. ReadPaper has explicit global guards and state transitions before presenting SD as USB storage.

Decision: Put under Transfer, but require a confirm/modal state and stop reader/indexer first.

### Legado book identity

Problem: Legado progress save uses `name + author + durChapterIndex + durChapterPos + durChapterTime + durChapterTitle`, while `getBookshelf` books also have `bookUrl`. For local Vink books, matching by only name/author can collide.

Decision: Store a local mapping:

```text
Vink local book path/hash -> Legado bookUrl + name + author
```

If mapping is missing, ask user to pair/select once.

### Legado remote content vs local reading

Options:

1. Use Legado only for bookshelf/progress sync, keep local files as source of truth.
2. Stream chapters from Legado via `/getBookContent` and cache them locally.

Recommendation: start with option 1 for v0.3.0; add remote-content reading later.

### ReadPaper UI effects / shutter effects

ReadPaper has visual push effects and slicing. They are optional and not part of Vink UI identity.

Decision: keep the display queue capability, but default Vink UI to simple full/card refreshes first.

## Suggested Implementation Order

1. Finish v0.3 tab shell and hit-test routing.
2. Add state handler split: `handleReader`, `handleLibrary`, `handleTransfer`, `handleSettings`, `handleLegadoSync`.
3. Port timer/device task behavior: touch, battery, orientation, sleep.
4. Port font/text engine.
5. Port local book/page pipeline.
6. Implement local progress save.
7. Add Legado connection config and `GET /getBookshelf` test.
8. Add `POST /saveBookProgress`.
9. Add optional pull progress / conflict resolution.
10. Only after that consider remote chapter content.
