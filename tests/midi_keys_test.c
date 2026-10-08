/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#define main hostsim_main
#include "hostsim.c"
#undef main

static void reset(void)
{
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    memset(midi_keys, 0, sizeof midi_keys);
    memset(input_refs, 0, sizeof input_refs);
    memset(input_arp, 0, sizeof input_arp);
    memset(note_owner, 0, sizeof note_owner);
    memset(midi_mod_refs, 0, sizeof midi_mod_refs);
    memset(midi_mod_mask, 0, sizeof midi_mod_mask);
    memset(midi_follow_active, 0, sizeof midi_follow_active);
    memset(midi_revoice_bits, 0, sizeof midi_revoice_bits);
    memset(midi_revoice_word, 0, sizeof midi_revoice_word);
    midi_revoice_dirty = midi_revoice_pending = midi_revoice_next = 0;
    memset(kb_kind, 0, sizeof kb_kind);
    memset(vl_n, 0, sizeof vl_n);
    memset(stq, 0, sizeof stq);
    memset(mo_set, 0, sizeof mo_set);
    host_tracks_init();
    midi_follow_scl = 0;
    fm1_in.notes = fm1_in.buttons = kb_prev = 0;
    mi_w = mi_r = mo_w = mo_r = 0;
    clk_pos = clk_beat = 0;
    panic_req = transport_req = 0;
    ft_on = ci_on = rec_wait = 0;
    usb.config = 1;
}

static void packet(uint32_t st, uint32_t note, uint32_t vel)
{
    assert(mi_w - mi_r < MQ);
    midi_in_q[mi_w++ % MQ] = (st >> 4) | st << 8 | note << 16 | vel << 24;
    events_block(CTL);
}

static int gate(uint32_t part, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (trk[part].v[i].gate && trk[part].v[i].note == note)
            return 1;
    return 0;
}

static void no_holds(void)
{
    uint32_t p, n, i;
    for (p = 0; p < NPART; p++) {
        assert(trk[p].arp_phys == 0 && trk[p].nheld == 0);
        for (n = 0; n < 128u; n++)
            assert(input_refs[p][n] == 0);
        for (i = 0; i < NVOICE; i++)
            assert(!trk[p].v[i].gate);
    }
    for (i = 0; i < STQ; i++)
        assert(!stq[i].on);
}

static void raw_and_routing(void)
{
    uint32_t ch;
    reset();
    assert(!midi_follow_scl);
    for (ch = 0; ch < 16u; ch++) {
        uint32_t part = ch < 3u ? ch : 2u;
        song.sel = 2;
        trk[part].p[P_QUANT] = 2;
        trk[part].p[P_SCALE] = 2;
        trk[part].p[P_CHORD] = 3;
        trk[part].p[P_TRANS] = 12;
        song.octave = 1;
        if (ch == 9u)
            continue;
        packet(0x90u | ch, 61, 73);
        assert(gate(part, 61));
        assert(midi_keys[ch][61].n == 1 && midi_keys[ch][61].vel == 73);
        song.sel = 0;
        song.g[G_DRCH] = 5;             /* note-off must retain even the old drum-channel decision */
        midi_follow_scl = 1;
        packet(0x80u | ch, 61, 32);
        no_holds();
        midi_follow_scl = 0;
        song.g[G_DRCH] = 10;
    }
    assert(mo_w == 0);                  /* external notes are never echoed */
    puts("midi keys: RAW is chromatic; all synth channels retain destination and mode on release");
}

static void mappings(void)
{
    uint32_t scale, root, mode, note;
    int oct, trans;
    track_t *t = &trk[0];
    reset();
    midi_follow_scl = 1;
    for (scale = 0; scale < NSCALES; scale++)
        for (root = 0; root < 12u; root++)
            for (mode = 0; mode < 3u; mode++) {
                t->p[P_SCALE] = (int16_t)scale;
                t->p[P_ROOT] = (int16_t)root;
                t->p[P_QUANT] = (int16_t)mode;
                for (oct = -3; oct <= 3; oct += 3)
                    for (trans = -24; trans <= 24; trans += 24) {
                        song.octave = (int8_t)oct;
                        t->p[P_TRANS] = (int16_t)trans;
                        for (note = 0; note < 128u; note++) {
                            uint32_t want = synth_key_map(t, (int32_t)note);
                            midi_key_t *key = &midi_keys[0][note];
                            midi_keys_event(0, note, 91);
                            assert(key->n == (want == KB_SILENT ? 0u : 1u));
                            if (key->n)
                                assert(key->notes[0] == want);
                            midi_keys_event(0, note, 0);
                        }
                    }
                for (note = 0; note < 27u; note++)
                    assert(synth_key_map(t, (int32_t)(53u + note)) == kb_map(t, note));
            }
    no_holds();
    reset();
    midi_follow_scl = 1;
    t->p[P_SCALE] = 2;
    t->p[P_QUANT] = 2;
    packet(0x90, 64, 91);               /* E -> Eb, accidental silent */
    assert(gate(0, 63));
    packet(0x90, 63, 91);
    assert(midi_keys[0][63].kind == MK_SILENT);
    packet(0x80, 63, 0);
    assert(gate(0, 63));
    packet(0x80, 64, 0);
    t->p[P_SCALE] = 5;                  /* pentatonic: each white key advances one degree */
    assert(synth_key_map(t, 71) == 74 && synth_key_map(t, 72) == 76);
    assert(synth_key_map(t, 59) == 57 && synth_key_map(t, 60) == 60);
    t->p[P_SCALE] = 2;
    t->p[P_QUANT] = 1;
    packet(0x90, 64, 80);
    packet(0x90, 63, 90);               /* two inputs collapse to Eb */
    assert(input_refs[0][63] == 2);
    packet(0x80, 64, 0);
    assert(gate(0, 63));
    packet(0x90, 63, 0);               /* velocity zero is note-off */
    no_holds();
    puts("midi keys: all scales/roots/modes/pitches, octave/transpose bounds; WHITE/SNAP collisions");
}

static void chords(void)
{
    uint32_t type, mod, i;
    static const uint8_t MODS[] = {66, 68, 70, 61, 63};
    track_t *t = &trk[0];
    reset();
    midi_follow_scl = 1;
    t->p[P_SCALE] = 1;
    for (type = 1; type <= 5u; type++)
        for (mod = 0; mod < 32u; mod++) {
            uint8_t expected[4];
            uint32_t n;
            t->p[P_CHORD] = (int16_t)type;
            for (i = 0; i < 5u; i++)
                if ((mod >> i) & 1u)
                    packet(0x90, MODS[i], 99);
            n = chord_play_notes(t, 60, mod, expected);
            {
                uint32_t a, b, kept = 0;
                for (a = 0; a < n; a++) {
                    for (b = 0; b < kept && expected[b] != expected[a]; b++)
                        ;
                    if (b == kept)
                        expected[kept++] = expected[a];
                }
                n = kept;             /* SUS4 modifier on SUS4 can duplicate a degree upstream */
            }
            packet(0x90, 60, 83);
            assert(midi_keys[0][60].n == n);
            assert(!memcmp(midi_keys[0][60].notes, expected, n));
            for (i = 0; i < n; i++)
                assert(gate(0, expected[i]));
            packet(0x80, 60, 0);
            for (i = 0; i < 5u; i++)
                if ((mod >> i) & 1u)
                    packet(0x80, MODS[i], 0);
            no_holds();
        }
    t->p[P_CHORD] = 1;
    packet(0x90, 60, 77);
    packet(0x90, 66, 99);               /* revoice held C major -> minor */
    assert(gate(0, 63) && !gate(0, 64));
    packet(0x80, 66, 0);
    assert(gate(0, 64) && !gate(0, 63));
    packet(0x90, 67, 70);               /* overlapping G in C and G chords */
    assert(input_refs[0][67] == 2);
    packet(0x80, 60, 0);
    assert(gate(0, 67));
    packet(0x80, 67, 0);
    no_holds();
    t->p[P_VLEAD] = 1;
    vl_n[0] = 0;
    packet(0x90, 60, 88);
    packet(0x80, 60, 0);
    packet(0x90, 65, 88);
    assert(midi_keys[0][65].notes[0] == 60);
    packet(0x80, 65, 0);
    t->p[P_VLEAD] = 0;
    t->p[P_STRUM] = 20;
    packet(0x90, 60, 88);
    assert(gate(0, 60) && !gate(0, 64));
    packet(0x80, 60, 0);               /* cancel queued strum notes */
    strum_block(FS);
    no_holds();
    for (i = 0; i < 128u; i++) {       /* high-edge CHORD+ never indexes outside MIDI notes */
        packet(0x90, 70, 99);
        packet(0x90, i, 88);
        packet(0x80, i, 0);
        packet(0x80, 70, 0);
    }
    no_holds();
    puts("midi keys: all CHORD types/modifier combinations, revoice, shared tones, VLEAD and STRUM");
}

static void lifecycle(void)
{
    uint32_t ch;
    track_t *t = &trk[1];
    reset();
    midi_follow_scl = 1;
    song.sel = 1;
    t->p[P_SCALE] = 2;
    t->p[P_CHORD] = 1;
    packet(0x93, 60, 100);
    t->p[P_SCALE] = 9;
    t->p[P_ROOT] = 6;
    t->p[P_TRANS] = 12;
    t->p[P_CHORD] = 5;
    song.octave = 2;
    song.sel = 2;
    song.g[G_DRCH] = 4;
    midi_follow_scl = 0;
    packet(0x83, 60, 0);
    no_holds();
    reset();
    midi_follow_scl = 1;
    trk[0].p[P_QUANT] = 1;
    trk[0].p[P_SCALE] = 2;
    trk[0].p[P_AMODE] = 1;
    packet(0x90, 64, 100);
    packet(0x90, 64, 100);              /* repeated identical input replaces, does not leak arp_phys */
    packet(0x93, 63, 100);              /* other channel, same resulting pitch */
    assert(trk[0].arp_phys == 1 && trk[0].nheld == 1);
    packet(0x80, 64, 0);
    assert(trk[0].arp_phys == 1 && trk[0].nheld == 1);
    packet(0x83, 63, 0);
    arp_tick(&trk[0], CTL * (uint32_t)song.g[G_BPM]);
    no_holds();
    trk[0].p[P_AHOLD] = 1;
    packet(0x90, 60, 100);
    packet(0xB0, 123, 0);
    no_holds();
    trk[0].p[P_AMODE] = trk[0].p[P_AHOLD] = 0;
    for (ch = 0; ch < 16u; ch++)
        if (ch != 9u)
            packet(0x90u | ch, 60, 100);
    for (ch = 0; ch < 16u; ch++)
        packet(0xB0u | ch, 120, 0);
    no_holds();
    packet(0x90, 60, 100);
    panic_req = 1;
    events_block(CTL);
    packet(0x80, 60, 0);
    no_holds();
    packet(0x90, 60, 100);
    song.g[G_ROUTE] = 1;
    packet(0x90, 62, 100);
    assert(midi_keys[0][62].kind == MK_NONE);
    packet(0x80, 60, 0);
    no_holds();
    puts("midi keys: held setting/routing changes, retrigger, arp, channel panic, track panic and CLOCK-only");
}

static void follow_routing_and_limits(void)
{
    uint32_t ch, i, seed = 17;
    reset();
    for (i = 48; i < 56u; i++)
        packet(0x90, i, 100);
    packet(0x91, 72, 100);
    assert(busy_now() == NVOICE);
    for (i = 0; i < NVOICE && trk[0].v[i].stage != 4u; i++)
        ;
    assert(i < NVOICE);
    packet(0x90, trk[0].v[i].note, 100); /* resurrecting a stolen pitch still needs budget room */
    assert(busy_now() == NVOICE);
    packet(0xB0, 123, 0);
    packet(0xB1, 123, 0);
    no_holds();
    reset();
    midi_follow_scl = 1;
    for (i = 0; i < NPART; i++) {
        trk[i].p[P_SCALE] = 2;
        trk[i].p[P_QUANT] = 2;
    }
    for (ch = 0; ch < 16u; ch++) {
        uint32_t part = ch < 3u ? ch : 2u;
        if (ch == 9u)
            continue;
        song.sel = 2;
        packet(0x90u | ch, 64, 94);
        assert(gate(part, 63));
        song.sel = 0;
        packet(0x80u | ch, 64, 0);
        no_holds();
    }
    song.g[G_DRCH] = 1;                 /* configured drum channel wins even over synth channel 1 */
    packet(0x90, 36, 99);
    assert(midi_keys[0][36].kind == MK_DRUM);
    packet(0x80, 36, 0);
    song.g[G_DRCH] = 10;
    for (i = 0; i < 6000u; i++) {
        uint32_t note, part;
        seed = seed * 1664525u + 1013904223u;
        ch = (seed >> 8) & 15u;
        note = (seed >> 16) & 127u;
        song.sel = (seed >> 24) % NTRK;
        midi_follow_scl = (seed >> 4) & 1u;
        part = (seed >> 5) % NPART;
        trk[part].p[P_SCALE] = (int16_t)((seed >> 2) % NSCALES);
        trk[part].p[P_ROOT] = (int16_t)((seed >> 12) % 12u);
        trk[part].p[P_QUANT] = (int16_t)(seed % 3u);
        trk[part].p[P_CHORD] = (int16_t)((seed >> 19) % 6u);
        trk[part].p[P_STRUM] = (int16_t)((seed >> 25) % 41u - 20);
        packet((seed & 1u ? 0x90u : 0x80u) | ch, note, 1u + (seed >> 9) % 127u);
        assert(busy_now() <= NVOICE);
        if (!(i % 37u))
            packet(0xB0u | ch, 123, 0);
    }
    for (ch = 0; ch < 16u; ch++)
        packet(0xB0u | ch, 123, 0);
    strum_block(FS);
    no_holds();
    for (i = V_POLY; i <= V_UNISON; i++) {
        reset();
        midi_follow_scl = 1;
        trk[0].p[P_SCALE] = 2;
        trk[0].p[P_CHORD] = 1;
        trk[0].p[P_VOICE] = (int16_t)i;
        packet(0x90, 60, 100);
        trk[0].p[P_VOICE] = (int16_t)((i + 1u) % 4u);
        packet(0x80, 60, 0);
        no_holds();
    }
    puts("midi keys: FOLLOW channel routing, drum-channel priority, 6000-event lifecycle/voice-budget stress");
}

static void drums_and_recording(void)
{
    uint32_t before, note;
    track_t *t = &trk[0];
    reset();
    midi_follow_scl = 1;
    song.playing = 1;
    song.rec = 1u << TRK_DRUM;
    packet(0x99, 36, 110);
    assert(dstep_lvl(&TDRUM->dstep[0], lane_of_note(36)) == vel_lvl(110));
    assert(midi_keys[9][36].kind == MK_DRUM);
    packet(0x89, 36, 0);
    song.sel = TRK_DRUM;                /* selected-track routing to drums is also unchanged */
    packet(0x93, 38, 75);
    assert(dstep_lvl(&TDRUM->dstep[0], lane_of_note(38)) == vel_lvl(75));
    reset();
    midi_follow_scl = 1;
    song.playing = song.rec = 1;
    t->p[P_SCALE] = 2;
    t->p[P_CHORD] = 1;
    packet(0x90, 60, 81);
    assert(t->step[0].n == 3 && t->step[0].note[0] == 60 &&
        t->step[0].note[1] == 63 && t->step[0].note[2] == 67 && t->step[0].vel == 81);
    packet(0x80, 60, 0);
    song.rec = 0;
    t->p[P_ROOT] = 6;
    t->p[P_TRANS] = 12;
    t->p[P_CHORD] = 5;
    t->seq_abs = SEQ_NONE;
    t->rskip_n = 0;
    clk_pos = clk_beat = 0;
    seq_tick(t, CTL * 120u);
    assert(gate(0, 60) && gate(0, 63) && gate(0, 67)); /* playback uses stored sounding pitches */
    seq_stop();
    reset();
    midi_follow_scl = 1;
    song.playing = song.rec = 1;
    t->p[P_SCALE] = 2;
    t->p[P_QUANT] = 2;
    t->p[P_AMODE] = 1;
    packet(0x90, 64, 81);
    assert(t->step[0].n == 1 && t->step[0].note[0] == 63); /* arp records what it sounds */
    packet(0x80, 64, 0);
    seq_stop();
    reset();
    midi_follow_scl = 1;
    before = mo_w;
    um_byte(0x93); um_byte(64); um_byte(82);
    trk[0].p[P_SCALE] = 2;
    trk[0].p[P_QUANT] = 2;
    events_block(CTL);
    assert(gate(0, 63));
    um_byte(0x83); um_byte(64); um_byte(0);
    events_block(CTL);
    assert(mo_w == before);
    no_holds();
    midi_in_event(0x09u | 0x90u << 8 | 64u << 16 | 82u << 24);
    events_block(CTL);
    assert(gate(0, 63));
    midi_in_event(0x08u | 0x80u << 8 | 64u << 16);
    events_block(CTL);
    no_holds();
    if (drum_set() >= 0) {
        t->eng_req = t->engine = 4;
        t->p[P_E0] = (int16_t)drum_set();
        t->p[P_CHORD] = 1;
        for (note = 36; note < 48u; note++) {
            packet(0x90, note, 100);
            assert(midi_keys[0][note].kind == MK_RAW && midi_keys[0][note].notes[0] == note);
            packet(0x80, note, 0);
        }
    }
    puts("midi keys: drums, resulting-pitch recording/playback, arp recording, real TRS parser and GM-kit bypass");
}

static void source_overlaps(void)
{
    track_t *t = &trk[0];
    reset();
    fm1_in.notes = 1u << 7;
    keyboard_block();
    packet(0x90, 60, 90);
    fm1_in.notes = 0;
    keyboard_block();
    assert(gate(0, 60));                /* onboard release must not end external MIDI */
    packet(0x80, 60, 0);
    no_holds();
    reset();
    packet(0x90, 60, 90);
    fm1_in.notes = 1u << 7;
    keyboard_block();
    packet(0x80, 60, 0);
    assert(gate(0, 60));                /* external release must not end the onboard key */
    fm1_in.notes = 0;
    keyboard_block();
    no_holds();
    reset();
    packet(0x90, 60, 90);
    t->step[0].n = 1;
    t->step[0].note[0] = 60;
    t->step[0].vel = 100;
    t->step[0].time = ST_NOTE;
    seq_step(t, &t->step[0], BEAT_U, 0);
    seq_release(t);
    assert(gate(0, 60));                /* sequence gate must not end a physically held input */
    packet(0x80, 60, 0);
    no_holds();
    reset();
    packet(0x90, 60, 90);
    t->p[P_AMODE] = 1;
    packet(0x93, 60, 90);               /* arp output overlaps a pre-ARP direct input */
    t->arp_off = 1;
    arp_tick(t, CTL * 120u);
    assert(gate(0, 60));
    packet(0x83, 60, 0);
    packet(0x80, 60, 0);
    no_holds();
    reset();
    packet(0x90, 60, 90);
    t->step[0].n = 1;
    t->step[0].note[0] = 60;
    t->step[0].vel = 100;
    t->step[0].time = ST_NOTE;
    seq_step(t, &t->step[0], BEAT_U, 0);
    packet(0x80, 60, 0);
    assert(gate(0, 60));                /* live release must not end sequence playback either */
    seq_release(t);
    no_holds();
    reset();
    packet(0x90, 60, 90);
    roll_start(0, t, 60, 3);
    roll_start(1, t, 60, 3);
    roll_end(0);
    midi_keys_event(0, 60, 0);           /* isolate roll ownership from keyboard layer cleanup */
    assert(gate(0, 60));
    roll_end(1);
    no_holds();
    reset();
    t->p[P_AMODE] = 1;
    packet(0x90, 0, 90);
    assert(gate(0, 0) && trk_note_owned(t, 0, NOTE_ARP));
    packet(0x80, 0, 0);
    no_holds();
    reset();
    midi_follow_scl = 1;
    t->p[P_SCALE] = t->p[P_CHORD] = 1;
    t->p[P_STRUM] = 20;
    packet(0x90, 60, 90);
    midi_follow_scl = 0;
    packet(0x93, 64, 90);
    packet(0x80, 60, 0);                /* queued strum's release respects another live owner */
    assert(gate(0, 64));
    packet(0x83, 64, 0);
    strum_block(FS);
    no_holds();
    reset();
    t->step[0].n = 1;
    t->step[0].note[0] = 64;
    t->step[0].vel = 100;
    t->step[0].time = ST_NOTE;
    seq_step(t, &t->step[0], BEAT_U, 0);
    midi_follow_scl = 1;
    t->p[P_SCALE] = t->p[P_CHORD] = 1;
    t->p[P_STRUM] = 20;
    packet(0x90, 60, 90);
    packet(0x80, 60, 0);
    assert(gate(0, 64));
    {
        uint32_t age = vage, i;
        for (i = 0; i < STQ; i++)
            assert(!stq[i].on);          /* release cancels only its delayed source, not the shared voice */
        strum_block(FS);
        assert(vage == age && gate(0, 64));
    }
    seq_release(t);
    no_holds();
    reset();
    packet(0x90, 60, 90);
    trk_all_off(t);                     /* scene/preset panic invalidates saved MIDI mappings */
    packet(0x93, 60, 90);
    packet(0x80, 60, 0);
    assert(gate(0, 60) && input_refs[0][60] == 1);
    packet(0x83, 60, 0);
    no_holds();
    puts("midi keys: onboard/external/sequence/arp ownership overlaps");
}

static void bounded_work(void)
{
    static const uint8_t keys[] = {36,38,40,41,43,45,47,48,50,52,53,55,57,59,60,62,64,65,67,69};
    uint8_t before[sizeof keys][4];
    uint32_t i, changed, blocks, ch, note;
    track_t *t = &trk[0];
    reset();
    midi_follow_scl = 1;
    t->p[P_SCALE] = t->p[P_CHORD] = 1;
    for (i = 0; i < sizeof keys; i++) {
        packet(0x90, keys[i], 90);
        memcpy(before[i], midi_keys[0][keys[i]].notes, 4);
    }
    packet(0x90, 66, 90);
    for (changed = i = 0; i < sizeof keys; i++)
        changed += memcmp(before[i], midi_keys[0][keys[i]].notes, 4) != 0;
    assert(changed == MIDI_REVOICE_BUDGET);
    /* Continuous modifier changes must not restart a pass and starve late keys. */
    for (blocks = 0; blocks < 6u; blocks++)
        packet(blocks & 1u ? 0x90 : 0x80, 66, blocks & 1u ? 90 : 0);
    packet(0x90, 66, 90);
    for (blocks = 0; midi_revoice_pending || midi_revoice_dirty; blocks++) {
        assert(blocks < 20u);
        midi_keys_block();
    }
    for (i = 0; i < sizeof keys; i++)
        assert(memcmp(before[i], midi_keys[0][keys[i]].notes, 4));
    packet(0x93, 66, 90);               /* releasing one of two modifier owners changes nothing */
    assert(midi_mod_refs[0][0] == 2 && midi_key_mods(0) == 1);
    packet(0x80, 66, 0);
    assert(midi_mod_refs[0][0] == 1 && midi_key_mods(0) == 1);
    packet(0x83, 66, 0);
    assert(!midi_key_mods(0));
    packet(0x80, keys[19], 0);           /* cancellation of unfinished revoice work */
    packet(0x90, keys[19], 90);          /* a new identity must not inherit the canceled job */
    assert(!(midi_revoice_bits[0][keys[19] >> 5] & (1u << (keys[19] & 31u))));
    for (blocks = 0; midi_revoice_pending || midi_revoice_dirty; blocks++) {
        assert(blocks < 20u);
        midi_keys_block();
    }
    packet(0xB0, 123, 0);
    no_holds();
    reset();
    input_arp_reset(TDRUM);              /* REC-hold clear may target the drum track */
    for (i = 0; i < 20u; i++)
        midi_in_q[mi_w++ % MQ] = 9u | 0x90u << 8 | (40u + i) << 16 | 90u << 24;
    events_block(CTL);
    assert(mi_r == 8u && mi_w == 20u);
    events_block(CTL);
    assert(mi_r == 16u);
    events_block(CTL);
    assert(mi_r == mi_w);
    packet(0xB0, 123, 0);
    no_holds();
    reset();
    for (ch = 0; ch < 16u; ch++)
        for (note = 0; note < 128u; note++)
            midi_keys_event(ch, note, 90);
    assert(input_refs[0][60] == 13 && input_refs[1][60] == 1 && input_refs[2][60] == 1);
    for (ch = 0; ch < 16u; ch++)
        midi_keys_channel_off(ch);
    no_holds();
    puts("midi keys: 4 revoice visits/block, no starvation, cancellation, cached modifiers, 8 packets/block, all 2048 slots");
}

static void onboard_chord_holds(void)
{
    static const uint8_t modifiers[] = {13, 15, 17, 8, 10};
    uint32_t scale, type, mods, i;
    for (scale = 0; scale < NSCALES; scale++)
        for (type = 1; type <= 5u; type++)
            for (mods = 0; mods < 32u; mods++) {
                reset();
                trk[0].p[P_SCALE] = (int16_t)scale;
                trk[0].p[P_CHORD] = (int16_t)type;
                fm1_in.notes = 1u << 7;
                keyboard_block();
                for (i = 0; i < 5u; i++)
                    if ((mods >> i) & 1u)
                        fm1_in.notes |= 1u << modifiers[i];
                keyboard_block();
                fm1_in.notes = 1u << 7;
                keyboard_block();
                fm1_in.notes = 0;
                keyboard_block();
                no_holds();
            }
    puts("midi keys: shared input counts preserve every onboard CHORD/modifier lifecycle");
}

int main(void)
{
    source_overlaps();
    bounded_work();
    onboard_chord_holds();
    raw_and_routing();
    mappings();
    chords();
    lifecycle();
    follow_routing_and_limits();
    drums_and_recording();
    reset();
    assert(trs_test() == 0);            /* existing RAW routing/live-recording regression */
    puts("midi keys: all checks passed");
    return 0;
}
