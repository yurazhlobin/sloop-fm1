/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by seq.c: external notes share musical helpers, not local UI layers.
 * Runtime-only setting: no project, preset or settings format changes. */
static volatile uint8_t midi_follow_scl;
enum { MK_NONE, MK_RAW, MK_FOLLOW, MK_MOD, MK_SILENT, MK_DRUM };
typedef struct {
    uint8_t kind, trk, root, vel, n, notes[4];
} midi_key_t;
/* Every channel/pitch has a slot, including silent WHITE keys. Large state goes
 * in the existing zero-initialised pool, not the 96 KiB small-data RAM. */
static midi_key_t midi_keys[16][128] __attribute__((section(".pool")));
static uint16_t midi_key_refs[NPART][128];

static uint32_t midi_key_mods(uint32_t part)
{
    uint32_t ch, note, mods = 0;
    for (ch = 0; ch < 16u; ch++)
        for (note = 0; note < 128u; note++)
            if (midi_keys[ch][note].kind == MK_MOD && midi_keys[ch][note].trk == part)
                mods |= chord_mod_of_note(note);
    return mods;
}

static void midi_key_off(midi_key_t *key)
{
    uint32_t i;
    track_t *t = &trk[key->trk];
    for (i = 0; i < key->n; i++) {
        uint16_t *refs = &midi_key_refs[key->trk][key->notes[i]];
        if (*refs && !--*refs)
            input_off(t, key->notes[i]);
    }
    key->n = 0;
}

static void midi_key_on(midi_key_t *key, uint32_t i)
{
    track_t *t = &trk[key->trk];
    uint16_t *refs = &midi_key_refs[key->trk][key->notes[i]];
    /* One arp/recording hold per sounding pitch. Polyphonic collisions may
     * retrigger, but the last owning input is the one that releases the pitch. */
    if (!(*refs)++ || !t->p[P_AMODE]) {
        in_chord = key->n > 1u ? key->notes : 0;
        in_chord_n = key->n;
        in_chord_i = i;
        input_on(t, key->notes[i], key->vel);
        in_chord = 0;
    }
}

static uint32_t midi_key_chord(track_t *t, uint32_t root, uint32_t mods, uint8_t *notes)
{
    uint32_t n = chord_play_notes(t, root, mods, notes), i, j, kept = 0;
    for (i = 0; i < n; i++) {
        if (notes[i] > 127u)
            continue;
        for (j = 0; j < kept && notes[j] != notes[i]; j++)
            ;
        if (j == kept)
            notes[kept++] = notes[i];
    }
    return kept;
}

static void midi_key_revoice(uint32_t part)
{
    uint32_t ch, note, mods = midi_key_mods(part), i, j;
    track_t *t = &trk[part];
    if (!t->p[P_CHORD])
        return;
    for (ch = 0; ch < 16u; ch++)
        for (note = 0; note < 128u; note++) {
            midi_key_t *key = &midi_keys[ch][note];
            uint8_t next[4];
            uint32_t n;
            if (key->kind != MK_FOLLOW || key->trk != part)
                continue;
            n = midi_key_chord(t, key->root, mods, next);
            for (i = 0; i < key->n; i++) {
                for (j = 0; j < n && next[j] != key->notes[i]; j++)
                    ;
                if (j == n) {
                    uint16_t *refs = &midi_key_refs[part][key->notes[i]];
                    if (*refs && !--*refs)
                        input_off(t, key->notes[i]);
                }
            }
            for (j = 0; j < n; j++) {
                for (i = 0; i < key->n && key->notes[i] != next[j]; i++)
                    ;
                if (i == key->n) {
                    uint16_t *refs = &midi_key_refs[part][next[j]];
                    if (!(*refs)++)
                        input_on(t, next[j], key->vel);
                }
            }
            memcpy(key->notes, next, n);
            key->n = (uint8_t)n;
        }
}

static int midi_key_bypass(const track_t *t)
{
    if (ENGINES[t->eng_req % NENGINES] == &ENG_SAMPLE && drum_set() >= 0 &&
        (uint32_t)t->p[P_E0] % SMP_NSETS == (uint32_t)drum_set())
        return 1;                                    /* GM notes are not keyboard scale degrees */
#if FELUCCA_SLICE
    if (ENGINES[t->eng_req % NENGINES] == &ENG_SLICE)
        return 1;                                    /* slice selectors stay chromatic */
#endif
    return 0;
}

static void midi_keys_event(uint32_t ch, uint32_t note, uint32_t vel)
{
    midi_key_t *key = &midi_keys[ch][note];
    uint32_t kind = key->kind, part = key->trk, i, n;
    track_t *t;
    if (kind != MK_NONE) {
        key->kind = MK_NONE;
        midi_key_off(key);                            /* same input retrigger replaces its old mapping */
        if (kind == MK_MOD)
            midi_key_revoice(part);
    }
    if (!vel)
        return;
    t = midi_track(ch);
    part = trk_index(t);
    key->trk = (uint8_t)part;
    key->vel = (uint8_t)vel;
    if (is_drum(t)) {
        key->kind = MK_DRUM;
        drum_input(lane_of_note(note), vel_lvl(vel), 0, 1);
        return;
    }
    key->kind = MK_RAW;
    n = note;
    if (midi_follow_scl && !midi_key_bypass(t)) {
        key->kind = MK_FOLLOW;
        n = synth_key_map(t, (int32_t)note);
        if (n == KB_SILENT) {
            key->kind = t->p[P_CHORD] ? MK_MOD : MK_SILENT;
            if (key->kind == MK_MOD)
                midi_key_revoice(part);
            return;
        }
    }
    key->root = (uint8_t)n;
    key->notes[0] = (uint8_t)n;
    key->n = (uint8_t)(key->kind == MK_FOLLOW && t->p[P_CHORD]
        ? midi_key_chord(t, n, midi_key_mods(part), key->notes) : 1u);
    for (i = 0; i < key->n; i++)
        midi_key_on(key, i);
}

static void midi_keys_channel_off(uint32_t ch)
{
    uint32_t note, parts = 0, i;
    for (note = 0; note < 128u; note++) {
        midi_key_t *key = &midi_keys[ch][note];
        if (key->kind != MK_NONE)
            parts |= 1u << key->trk;
        key->kind = MK_NONE;
        midi_key_off(key);
    }
    for (i = 0; i < NPART; i++)
        if ((parts >> i) & 1u) {
            track_t *t = &trk[i];
            if (!t->arp_phys) {                       /* channel panic also ends a latched arp */
                t->nheld = 0;
                if (t->arp_note) {
                    trk_note_off(t, t->arp_note);
                    seq_out_off(t, t->arp_note);
                    t->arp_note = 0;
                }
            }
            midi_key_revoice(i);
        }
}

static void midi_keys_panic(uint32_t parts)
{
    uint32_t ch, note;
    for (ch = 0; ch < 16u; ch++)
        for (note = 0; note < 128u; note++) {
            midi_key_t *key = &midi_keys[ch][note];
            if ((parts >> key->trk) & 1u) {
                key->kind = MK_NONE;
                midi_key_off(key);
            }
        }
}
