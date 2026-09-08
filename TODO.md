# Add BBC Radio (all stations) + now-playing

Scoping/research pass only - nothing implemented yet. Everything below marked "VERIFIED" was
tested for real (curl/browser, 2026-09-08) against BBC's live public endpoints from this dev
machine; everything marked "OPEN QUESTION" still needs a decision or more digging before writing
code. Source list: https://www.bbc.com/audio/stations (fetched via the in-app browser - a direct
`WebFetch` to bbc.com was blocked in this environment, so use a real browser/`curl` for any
follow-up, not `WebFetch`).

## 1. The station list - VERIFIED, 61 stations total

Read directly off https://www.bbc.com/audio/stations's rendered DOM (link hrefs), not guessed.
Every id below is what BBC Sounds itself uses (`bbc.co.uk/sounds/play/live/<id>`) and is ALSO the
`service_id` the now-playing API in section 2 expects - confirmed the same id works in both
places (e.g. `bbc_6music`, `bbc_radio_scotland_fm`).

**Featured / speech (2):**
- `bbc_world_service` - BBC World Service
- `bbc_radio_fourfm` - Radio 4 (FM feed; no separate LW id was on the current page - the old
  `radio4-lw` id from a 2016-era community list, see section 5, may be dead)

**UK National Stations (22):**
`bbc_radio_one`, `bbc_radio_one_anthems`, `bbc_radio_one_dance`, `bbc_1xtra`, `bbc_radio_two`,
`bbc_radio_three`, `bbc_radio_three_unwind`, `bbc_radio_four_extra`, `bbc_radio_five_live`,
`bbc_6music`, `bbc_asian_network`, `bbc_radio_scotland_fm`, `bbc_radio_scotland_mw`,
`bbc_radio_orkney`, `bbc_radio_shetland`, `bbc_radio_nan_gaidheal`, `bbc_radio_ulster`,
`bbc_radio_foyle`, `bbc_radio_wales_fm`, `bbc_radio_wales_am`, `bbc_radio_cymru`,
`bbc_radio_cymru_2`

**UK Local Stations (40):**
`bbc_radio_berkshire`, `bbc_radio_bristol`, `bbc_radio_cambridge`, `bbc_radio_cornwall`,
`bbc_radio_coventry_warwickshire`, `bbc_radio_cumbria`, `bbc_radio_derby`, `bbc_radio_devon`,
`bbc_radio_essex`, `bbc_radio_gloucestershire`, `bbc_radio_guernsey`,
`bbc_radio_hereford_worcester`, `bbc_radio_humberside`, `bbc_radio_jersey`, `bbc_radio_kent`,
`bbc_radio_lancashire`, `bbc_radio_leeds`, `bbc_radio_leicester`, `bbc_radio_lincolnshire`,
`bbc_london`, `bbc_radio_manchester`, `bbc_radio_merseyside`, `bbc_radio_newcastle`,
`bbc_radio_norfolk`, `bbc_radio_northampton`, `bbc_radio_nottingham`, `bbc_radio_oxford`,
`bbc_radio_sheffield`, `bbc_radio_shropshire`, `bbc_radio_solent`, `bbc_radio_solent_west_dorset`,
`bbc_radio_somerset_sound`, `bbc_radio_stoke`, `bbc_radio_suffolk`, `bbc_radio_surrey`,
`bbc_radio_sussex`, `bbc_tees`, `bbc_three_counties_radio`, `bbc_radio_wiltshire`, `bbc_wm`,
`bbc_radio_york`

**OPEN QUESTION - phase this in, don't add all 61 at once.** `station_list.c`'s `RADIO_STATIONS`
array is currently a short hand-picked list (see `station_list.h`) - adding 61 entries in one shot
massively grows NVS-persisted-index bounds checks, the AVRCP next/prev cycle length, and the
console/DP666 station-list UI all at once, for stations most of which are speech-only (no
now-playing track data at all - see section 2). Suggest: start with the music-heavy national
stations only (Radio 1/1Xtra/1Anthems/1Dance/2/3/6 Music/Asian Network - 8 stations, all confirmed
returning real music segment data in section 2's testing), get that working end-to-end and
hardware-tested, THEN decide whether the nations stations (Scotland/Wales/Ulster + their extras)
and the 40 local stations are worth adding - locals in particular tested as returning **zero**
music segments during a normal weekday evening (section 2), so they may add 40 list entries for
a "no artwork, ever" experience most of the time.

## 2. Now-playing / now-and-next - VERIFIED, real public JSON API found

BBC's own web player (and BBC Sounds) is backed by:

```
GET https://rms.api.bbc.co.uk/v2/services/<service_id>/segments/latest?experience=domestic&offset=0&limit=4
```

No API key, no auth, plain HTTPS GET, `Accept: application/json` not even required (tested with
just `User-Agent: Mozilla/5.0`). Confirmed working for a national music station
(`bbc_radio_two`), a nations station (`bbc_radio_scotland_fm`), and 6 Music. Response shape
(`data[]`, most-recent-first, `data[0]` is the currently-playing track):

```json
{
  "total": 4, "limit": 4, "offset": 0,
  "data": [
    {
      "id": "p0p8m9sr",
      "segment_type": "music",
      "titles": { "primary": "Bonobo", "secondary": "Rosewood", "tertiary": null, "entity_title": "Bonobo" },
      "image_url": "https://ichef.bbci.co.uk/images/ic/{recipe}/p09yt1z5.jpg",
      "offset": { "start": 4058, "end": 4291, "label": "Now Playing", "now_playing": true },
      "uris": []
    },
    { "...": "up to `limit` older segments follow, each with now_playing: false and a label like \"1 Minute Ago\"" }
  ]
}
```

Field mapping (this project's existing `nowplaying_info_t` convention, `nowplaying.h`):
- `titles.primary` -> artist (`subtitle`)
- `titles.secondary` -> track title (`title`)
- `image_url`, with `{recipe}` substituted -> `art_url`. **VERIFIED the `{recipe}` slot takes a
  plain `WxH` string, and `240x240` specifically works**
  (`curl -sI https://ichef.bbci.co.uk/images/ic/240x240/<id>.jpg` -> `HTTP 200 image/jpeg`) - same
  "request the exact `RADIO_COMPANION_ART_W`x`_H` box" trick `art_fallback.c`'s `itunes_lookup()`
  already uses, and it would land on `album_art.c`'s exact-size fast path (no resize/postprocess)
  the same way. **BBC now-playing artwork should NOT go through `art_fallback.c` at all** - BBC
  already hands back the real, correct artwork URL directly, so this is a THIRD, independent
  now-playing source alongside `nowplaying_ingest_id3_tag()`/`nowplaying_ingest_icy_title()`, not
  a fallback consumer.
- `offset.now_playing: true` on `data[0]` is the reliable "this is actually live" signal - don't
  assume `data[0]` is always "now playing" without checking this field (a station between tracks
  or in a speech segment might have `data[0].now_playing: false` or an empty `data` array
  entirely - see the `bbc_radio_fourfm`/`bbc_london` results below).
- `segment_type: "music"` implies other segment types exist (speech/jingle/ad?) - only ever seen
  `"music"` in testing so far; worth logging any unrecognized `segment_type` rather than assuming
  it's always `"music"`.

**Coverage varies a lot by station - VERIFIED:**
- `bbc_radio_two`, `bbc_radio_scotland_fm`, `bbc_6music`: real music data, as expected.
- `bbc_radio_fourfm` (speech station): `"total": 0, "data": []` - correct/expected, not an error.
  Every purely-speech station (World Service, Radio 4, Radio 5 Live, local stations' talk shows)
  will look like this most of the time.
- `bbc_london` (a local station): `"total": 0, "data": []` at the time tested (a weekday evening -
  local stations are talk-heavy with only occasional music) - **not confirmed whether local
  stations ever return real data during their music segments, or whether the segments API simply
  isn't populated for local stations at all**. Test a local station during a daytime music slot
  before assuming this endpoint covers them at all.

**OPEN QUESTION - polling cadence / detecting a track change.** The user's own framing is right:
this needs polling, there's no push mechanism here (unlike AudD's callback product from the
Shazam-fingerprinting discussion earlier in this project's history - not relevant to BBC, just
noting the contrast). `offset.start`/`offset.end` are NOT wall-clock/epoch seconds (tested: values
like `4058`/`4291` don't correspond to seconds-since-midnight at the time of the request) - they
look session/stream-relative, but the exact epoch was NOT pinned down this session. Two options:
1. **Simple, safe first cut**: poll on a fixed interval (same cadence class as
   `RADIO_COMPANION_ART_POLL_INTERVAL_MS`, e.g. every 15-20s) and diff `data[0].id` (or the
   `titles` pair) against the last-seen value to detect a real track change - ignores
   `offset.end` entirely, costs a wasted request most polls but is trivial and correct.
2. **Smarter, later**: once `offset` semantics are actually pinned down, poll again shortly after
   the estimated `offset.end` instead of on a fixed timer (same spirit as
   `playlist_prefetch.c`'s "anticipate the boundary" approach to HLS live-window edges) - deferred
   until (1) is working and someone has time to reverse-engineer the offset base properly.

## 3. Stream URLs - use TuneIn (BBC's own page was scoped to metadata only, per section 2)

BBC's page/API was the source for now-playing metadata (section 2), not for the actual audio
stream - streams for a new BBC station should resolve the same way every existing station in
`station_list.c` already does, via TuneIn. This was checked directly rather than assumed:

**VERIFIED, and deliberately not pursued further**: BBC's own live-stream resolver
(`open.live.bbc.co.uk/mediaselector/...`) returned a response carrying an explicit legal notice
("this code and data form part of the BBC iPlayer content protection system... unauthorised use
... constitutes circumvention ... may result in legal action") before even returning a usable
result (`"result":"selectionunavailable"` for the URL shape tried). **Do not probe this endpoint
further** - unlike BBC's now-playing segments API (section 2, a plain public, undisclaimed JSON
endpoint) or Cover Art Archive/iTunes/Deezer/MusicBrainz (`art_fallback.c`), this one is
explicitly framed as DRM-protected infrastructure, not a public API.

The right path: **resolve BBC stream URLs via TuneIn**, exactly like every other station in
`station_list.c` already works (`tunein_control.c`'s existing profile/Tune.ashx flow) - TuneIn
indexes BBC stations under their own guide ids already (the same service this project already
depends on for every non-BBC station). This also means BBC stations get the SAME
`tunein_start_session()` code path as everything else, no new stream-resolution code needed at
all - only the now-playing SOURCE (section 2) and, if TuneIn's own quality/reliability for a given
BBC station turns out to be bad, a direct-bypass session builder in the style of
`start_direct_stream_session()`/`RADIO_VIBE_OF_VEGAS_*` in `app_config.h`/`tunein_control.c` (i.e.
find BBC's own *public*, non-DRM stream endpoint by hand for that one station specifically, the
same way that bypass was justified - not a general "reverse the whole mediaselector API" effort).

**OPEN QUESTION**: confirm each planned BBC station id actually resolves to a working
stream via TuneIn's `Tune.ashx` before assuming this path works for all of them - not tested this
session (this section stopped at "don't touch BBC's own protected resolver", not "confirmed
TuneIn definitely carries every BBC local station").

## 4. Integration sketch (not started)

- `station_list.h`/`.c`: add the chosen subset of BBC stations (see section 1's phasing note) -
  each needs a TuneIn guide id (section 3), not a BBC service id directly, for the existing
  `tunein_start_session()` path to work.
- New module, e.g. `bbc_nowplaying.c`/`.h`: polls `rms.api.bbc.co.uk` (section 2) for whichever
  station is currently selected, ONLY when that station is a BBC one (needs a way to know "this
  RADIO_STATIONS entry is BBC and its service_id is X" - probably a new field on `radio_station_t`
  in `station_list.h`, parallel to how `RADIO_VIBE_OF_VEGAS_STATION_ID` is special-cased today,
  but for N stations rather than one). Feeds results into the same shared `nowplaying_info_t`
  result cJSON/mutex-protected cache `nowplaying.c` already owns - a third producer alongside
  `nowplaying_ingest_id3_tag()` (CMAF/ID3) and `nowplaying_ingest_icy_title()` (ICY), e.g.
  `nowplaying_ingest_bbc_segment()`.
- Runs on its own small task/timer (own TLS session, occasional - same "extra concurrent TLS is
  fine if bounded/occasional" pattern `art_fallback.c`/`playlist_prefetch.c`/`album_art.c` already
  established), gated to only poll while a BBC station is actually the one currently playing.
- Reuses the User-Agent-spoofing convention `tunein_control.c`'s `http_get()` already uses
  (BBC's endpoints didn't complain about a plain `Mozilla/5.0` in testing, but matching the
  existing project convention costs nothing and is one less thing to debug later if that changes).

## 5. Secondary source (older, for cross-reference only)

An older (~2016-era) community-maintained id list exists at
https://github.com/radiodan/bbc-services-api (`services.json`) with 12 stations including
`5livesportsextra` and `radio4-lw`, which are NOT on the current `bbc.com/audio/stations` page
(section 1) - may be discontinued, or just not featured on that particular page. Worth a quick
check if 5 Live Sports Extra specifically is wanted, but don't trust this list over the live page
for anything else - it predates several of the ids actually confirmed working in section 1/2 by a
decade.
