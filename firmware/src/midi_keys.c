/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by seq.c: external notes share musical helpers, not local UI layers.
 * Runtime-only setting: no project, preset or settings format changes. */
static volatile uint8_t midi_follow_scl;
enum { MK_NONE, MK_RAW, MK_FOLLOW, MK_MOD, MK_SILENT, MK_DRUM };
typedef struct {
    uint8_t kind : 3, trk : 2, n : 3;
    uint8_t root, vel, notes[4];
} midi_key_t;
_Static_assert(sizeof(midi_key_t) == 7u, "MIDI snapshots must fit small-data RAM");
static midi_key_t midi_keys[16][128];
static uint16_t midi_mod_refs[NPART][5];
static uint8_t midi_mod_mask[NPART], midi_revoice_dirty, midi_revoice_pending, midi_revoice_next;
#define MIDI_REVOICE_WORDS 64u
#define MIDI_REVOICE_BUDGET 4u
static uint32_t midi_follow_active[MIDI_REVOICE_WORDS] __attribute__((section(".pool")));
static uint32_t midi_revoice_bits[NPART][MIDI_REVOICE_WORDS] __attribute__((section(".pool")));
static uint8_t midi_revoice_word[NPART];

static uint32_t midi_key_mods(uint32_t part)
{
    return midi_mod_mask[part];
}

static void midi_key_revoice(uint32_t part)
{
    midi_revoice_dirty |= (uint8_t)(1u << part);
}

static void midi_key_mod_change(uint32_t part, uint32_t note, int on)
{
    uint32_t bit = chord_mod_of_note(note), i = 0, old = midi_mod_mask[part];
    while ((1u << i) != bit)
        i++;
    if (on)
        midi_mod_refs[part][i]++;
    else if (midi_mod_refs[part][i])
        midi_mod_refs[part][i]--;
    if (midi_mod_refs[part][i])
        midi_mod_mask[part] |= (uint8_t)bit;
    else
        midi_mod_mask[part] &= (uint8_t)~bit;
    if (old != midi_mod_mask[part])
        midi_key_revoice(part);
}

static void midi_key_deactivate(uint32_t ch, uint32_t note)
{
    uint32_t idx = ch * 128u + note, bit = 1u << (idx & 31u), i;
    midi_follow_active[idx >> 5] &= ~bit;
    for (i = 0; i < NPART; i++)
        midi_revoice_bits[i][idx >> 5] &= ~bit;
}

static void midi_key_off(midi_key_t *key)
{
    uint32_t i;
    track_t *t = &trk[key->trk];
    for (i = 0; i < key->n; i++)
        input_off(t, key->notes[i]);
    key->n = 0;
}

static void midi_key_on(midi_key_t *key, uint32_t i)
{
    track_t *t = &trk[key->trk];
    in_chord = key->n > 1u ? key->notes : 0;
    in_chord_n = key->n;
    in_chord_i = i;
    input_on(t, key->notes[i], key->vel);
    in_chord = 0;
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

static void midi_key_revoice_one(midi_key_t *key)
{
    uint32_t part = key->trk, i, j, n;
    track_t *t = &trk[part];
    uint8_t next[4];
    n = midi_key_chord(t, key->root, midi_key_mods(part), next);
    for (i = 0; i < key->n; i++) {
        for (j = 0; j < n && next[j] != key->notes[i]; j++)
            ;
        if (j == n)
            input_off(t, key->notes[i]);
    }
    for (j = 0; j < n; j++) {
        for (i = 0; i < key->n && key->notes[i] != next[j]; i++)
            ;
        if (i == key->n)
            input_on(t, next[j], key->vel);
    }
    memcpy(key->notes, next, n);
    key->n = (uint8_t)n;
}

/* Coalesce modifier bursts, then visit at most four snapshots per audio block.
 * Cursors advance monotonically through 64 words; inactive slots are never
 * passed through chord generation. Releases remove pending work immediately. */
static void midi_keys_block(void)
{
    uint32_t part, budget = MIDI_REVOICE_BUDGET;
    for (part = 0; part < NPART; part++)
        if ((midi_revoice_dirty >> part) & 1u) {
            if (!trk[part].p[P_CHORD]) {
                midi_revoice_pending &= (uint8_t)~(1u << part);
                midi_revoice_dirty &= (uint8_t)~(1u << part);
            } else if (!((midi_revoice_pending >> part) & 1u)) {
                memcpy(midi_revoice_bits[part], midi_follow_active, sizeof midi_follow_active);
                midi_revoice_word[part] = 0;
                midi_revoice_pending |= (uint8_t)(1u << part);
                midi_revoice_dirty &= (uint8_t)~(1u << part);
            }
        }
    while (budget && midi_revoice_pending) {
        uint32_t word, bits, bit = 0, idx;
        midi_key_t *key;
        part = midi_revoice_next;
        midi_revoice_next = (uint8_t)((part + 1u) % NPART);
        if (!((midi_revoice_pending >> part) & 1u))
            continue;
        word = midi_revoice_word[part];
        while (word < MIDI_REVOICE_WORDS && !midi_revoice_bits[part][word])
            word++;
        midi_revoice_word[part] = (uint8_t)word;
        if (word == MIDI_REVOICE_WORDS) {
            midi_revoice_pending &= (uint8_t)~(1u << part);
            continue;
        }
        bits = midi_revoice_bits[part][word];
        while (!((bits >> bit) & 1u))
            bit++;
        midi_revoice_bits[part][word] &= ~(1u << bit);
        budget--;
        idx = word * 32u + bit;
        key = &midi_keys[idx >> 7][idx & 127u];
        if (key->kind == MK_FOLLOW && key->trk == part && trk[part].p[P_CHORD])
            midi_key_revoice_one(key);
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
        midi_key_deactivate(ch, note);
        key->kind = MK_NONE;
        midi_key_off(key);                            /* same input retrigger replaces its old mapping */
        if (kind == MK_MOD)
            midi_key_mod_change(part, note, 0);
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
                midi_key_mod_change(part, note, 1);
            return;
        }
    }
    key->root = (uint8_t)n;
    key->notes[0] = (uint8_t)n;
    key->n = (uint8_t)(key->kind == MK_FOLLOW && t->p[P_CHORD]
        ? midi_key_chord(t, n, midi_key_mods(part), key->notes) : 1u);
    if (key->kind == MK_FOLLOW) {
        uint32_t idx = ch * 128u + note;
        midi_follow_active[idx >> 5] |= 1u << (idx & 31u);
    }
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
        if (key->kind == MK_MOD)
            midi_key_mod_change(key->trk, note, 0);
        midi_key_deactivate(ch, note);
        key->kind = MK_NONE;
        midi_key_off(key);
    }
    for (i = 0; i < NPART; i++)
        if ((parts >> i) & 1u) {
            track_t *t = &trk[i];
            if (!t->arp_phys) {                       /* channel panic also ends a latched arp */
                t->nheld = 0;
                if (trk_note_owned(t, t->arp_note, NOTE_ARP)) {
                    trk_note_off_owned(t, t->arp_note, NOTE_ARP);
                    seq_out_off(t, t->arp_note);
                    t->arp_note = 0;
                }
            }
        }
}

static void midi_keys_panic(uint32_t parts)
{
    uint32_t ch, note;
    for (ch = 0; ch < 16u; ch++)
        for (note = 0; note < 128u; note++) {
            midi_key_t *key = &midi_keys[ch][note];
            if (key->kind != MK_NONE && ((parts >> key->trk) & 1u)) {
                midi_key_deactivate(ch, note);
                key->kind = MK_NONE;
                key->n = 0;
            }
        }
    for (ch = 0; ch < NPART; ch++)
        if ((parts >> ch) & 1u) {
            memset(midi_mod_refs[ch], 0, sizeof midi_mod_refs[0]);
            midi_mod_mask[ch] = 0;
            midi_revoice_dirty &= (uint8_t)~(1u << ch);
            midi_revoice_pending &= (uint8_t)~(1u << ch);
        }
}
