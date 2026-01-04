//spencer jackson
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midi.h"
#include "lv2_midi.h"
#include "lv2_audio.h"

#define INSTRUMENT_FILE_URI "http://github.com/ssj71/reMID.lv2/blob/master/instruments/instruments.conf"
#define POLYPHONY_URI "polyphony"
#define CHIPTYPE_URI "chip_type"
#define USE_SID_VOL_URI "use_sid_volume"

#define REMID_CHANNEL_STATE_URI "http://github.com/ssj71/reMID.lv2#channel_state"
#define REMID_CHANNEL_URI "http://github.com/ssj71/reMID.lv2#channel"
#define REMID_BANK_URI "http://github.com/ssj71/reMID.lv2#bank"
#define REMID_PROGRAM_URI "http://github.com/ssj71/reMID.lv2#program"
#define REMID_BANK_MSB_URI "http://github.com/ssj71/reMID.lv2#bank_msb"
#define REMID_BANK_LSB_URI "http://github.com/ssj71/reMID.lv2#bank_lsb"
#define REMID_BANK_NAME_URI "http://github.com/ssj71/reMID.lv2#bank_name"
#define REMID_PATCH_NAME_URI "http://github.com/ssj71/reMID.lv2#patch_name"

#define SND_SEQ_EVENT_NOTEOFF 0x80
#define SND_SEQ_EVENT_NOTEON 0x90
#define SND_SEQ_EVENT_KEYPRESS 0xa0
#define SND_SEQ_EVENT_CONTROLLER 0xb0
#define SND_SEQ_EVENT_PGMCHANGE 0xc0
#define SND_SEQ_EVENT_CHANPRESS 0xd0
#define SND_SEQ_EVENT_PITCHBEND 0xe0



static void notify_channel_states(struct lmidi* lm, midi_arrays_t* midi, int force_all)
{
    if (!lm || !midi) return;
    if (!lm->atom_out_p) return;

    struct super* owner = (struct super*)lm->owner;
    sw_bank_t* const* banks = owner ? owner->banks : NULL;

    for (int ch = 0; ch < 16; ++ch)
    {
        const midi_channel_state_t* st = &midi->midi_channels[ch];
        const uint8_t bank_id = (uint8_t)st->bank_id;
        const uint8_t program = (uint8_t)st->program;
        const uint8_t msb = (uint8_t)st->bank_msb;
        const uint8_t lsb = (uint8_t)st->bank_lsb;

        if (!force_all &&
            lm->last_bank_id[ch] == bank_id &&
            lm->last_program[ch] == program &&
            lm->last_bank_msb[ch] == msb &&
            lm->last_bank_lsb[ch] == lsb)
        {
            continue;
        }

        lm->last_bank_id[ch] = bank_id;
        lm->last_program[ch] = program;
        lm->last_bank_msb[ch] = msb;
        lm->last_bank_lsb[ch] = lsb;

        lv2_atom_forge_frame_time(&lm->forge, 0);

        LV2_Atom_Forge_Frame set_frame;
        lv2_atom_forge_object(&lm->forge, &set_frame, 0, lm->urid.p_Set);

        lv2_atom_forge_key(&lm->forge, lm->urid.p_property);
        lv2_atom_forge_urid(&lm->forge, lm->urid.remid_channel_state);

        lv2_atom_forge_key(&lm->forge, lm->urid.p_value);
        LV2_Atom_Forge_Frame val_frame;
        lv2_atom_forge_object(&lm->forge, &val_frame, 0, lm->urid.remid_channel_state);

        lv2_atom_forge_key(&lm->forge, lm->urid.remid_channel);
        lv2_atom_forge_int(&lm->forge, ch + 1);
        lv2_atom_forge_key(&lm->forge, lm->urid.remid_bank);
        lv2_atom_forge_int(&lm->forge, bank_id);
        lv2_atom_forge_key(&lm->forge, lm->urid.remid_program);
        lv2_atom_forge_int(&lm->forge, program);
        lv2_atom_forge_key(&lm->forge, lm->urid.remid_bank_msb);
        lv2_atom_forge_int(&lm->forge, msb);
        lv2_atom_forge_key(&lm->forge, lm->urid.remid_bank_lsb);
        lv2_atom_forge_int(&lm->forge, lsb);

        // Include human-friendly bank/patch names if we can resolve them.
        if (banks)
        {
            sw_bank_t* bank = NULL;
            if (bank_id < REMID_BANK_COUNT) bank = banks[bank_id];
            if (!bank) bank = banks[REMID_BANK_ALL];

            const char* bank_name = sw_bank_name(bank);
            char patch_buf[64];
            patch_buf[0] = 0;

            if (bank_name && *bank_name)
            {
                lv2_atom_forge_key(&lm->forge, lm->urid.remid_bank_name);
                lv2_atom_forge_string(&lm->forge, bank_name, (uint32_t)strlen(bank_name) + 1u);
            }

            if (bank && sw_bank_describe_program(bank, program, patch_buf, sizeof(patch_buf)))
            {
                lv2_atom_forge_key(&lm->forge, lm->urid.remid_patch_name);
                lv2_atom_forge_string(&lm->forge, patch_buf, (uint32_t)strlen(patch_buf) + 1u);
            }
        }

        lv2_atom_forge_pop(&lm->forge, &val_frame);
        lv2_atom_forge_pop(&lm->forge, &set_frame);
    }
}

void lv2_read_midi(void* mseq, uint32_t nframes, midi_arrays_t *midi)
{
    struct lmidi* lm = (struct lmidi*)mseq;
    LV2_Atom_Event event;
    uint8_t* msg;

    if(!lm || !midi) return;
    if(!lm->atom_in_p || !lm->atom_out_p) return;

    // Set up forge to write directly to notify output port.
    const uint32_t notify_capacity = lm->atom_out_p->atom.size;
    lv2_atom_forge_set_buffer(&lm->forge, (uint8_t*)lm->atom_out_p, notify_capacity);

    // Start a sequence in the notify output port.
    lv2_atom_forge_sequence_head(&lm->forge, &lm->atom_frame, 0);

    //tell host if we have a new file
    if(lm->newfilepath[0] == 1)
    {
     	lm->newfilepath[0] = 0;
		lv2_atom_forge_frame_time(&lm->forge, 0);
		LV2_Atom_Forge_Frame frame;
		lv2_atom_forge_object( &lm->forge, &frame, 0, lm->urid.p_Set);

		lv2_atom_forge_key(&lm->forge, lm->urid.p_property);
		lv2_atom_forge_urid(&lm->forge, lm->urid.filetype_instr);
		lv2_atom_forge_key(&lm->forge, lm->urid.p_value);
		lv2_atom_forge_path(&lm->forge, lm->filepath, strlen(lm->filepath)+1);

		lv2_atom_forge_pop(&lm->forge, &frame);
    }


    //TODO: not sample accurate
    LV2_ATOM_SEQUENCE_FOREACH(lm->atom_in_p, event)
    {
    	if(event)
    	{
    		if(event->body.type == lm->urid.m_midi_event)
    		{
                msg = (uint8_t*) LV2_ATOM_BODY(&(event->body));

				const uint32_t msg_size = event->body.size;
				uint8_t status = (msg_size > 0) ? msg[0] : 0;
				uint8_t param = (msg_size > 1) ? msg[1] : 0;
				uint8_t value = (msg_size > 2) ? msg[2] : 0;
				//printf("JACK MIDI event: %x %x %x\n", status, param, value);

				uint8_t ev_type = status&0xf0;
				uint8_t channel = status&0x0f;

				switch(ev_type)
				{
				case SND_SEQ_EVENT_CONTROLLER:
					if(!midi->midi_channels[channel].in_use) break;
					if(param==0 || param==32)
					{
						midi_bank_select_cc(midi, channel, param, value);
						break;
					}
					if(param==64)
					{
						if(value>64) midi->midi_channels[channel].sustain = 1;
						else midi->midi_channels[channel].sustain = 0;
					}
					else if(param==1)
					{
						// modulation controlling vibrato: value=0-127
						midi->midi_channels[channel].vibrato = value;
						//printf("%d\n", value);
						midi->midi_channels[channel].vibrato_changed = 1;
					}
					break;
				// case SND_SEQ_EVENT_KEYPRESS:
				case SND_SEQ_EVENT_CHANPRESS:
					if(!midi->midi_channels[channel].in_use) break;
					midi->midi_channels[channel].chanpress = value;
					midi->midi_channels[channel].chanpress_changed = 1;
					break;
				case SND_SEQ_EVENT_NOTEON:
					if(!midi->midi_channels[channel].in_use) break;
					note_on(midi, channel, param, value);
					break;
				case SND_SEQ_EVENT_NOTEOFF:
					if(!midi->midi_channels[channel].in_use) break;
					note_off(midi, channel, param);
					break;
				case SND_SEQ_EVENT_PITCHBEND:
					// value = -8192 to +8191
					if(!midi->midi_channels[channel].in_use) break;
					//int pitchbend = (value*128)|(param&0x7f);
					int pitchbend = (((value&0x7f)<<7)|(param&0x7f))-8192;
					//printf("got pitchbend %x %x: %x %d\n", param, value, pitchbend, pitchbend);
					midi->midi_channels[channel].pitchbend = pitchbend;
					break;
				case SND_SEQ_EVENT_PGMCHANGE:
					if(lm->chan_program_override[channel] && *lm->chan_program_override[channel] > 0.5f)
					{
						break;
					}
					if(midi->midi_channels[channel].program==-1) break;
					//printf("prg change %d\n", value);
					midi_set_program(midi, channel, (int)param);
					break;
				}//switch message type
    		}//if event is midi
    		else if(event->body.type == lm->urid.a_object)
    		{
				const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&event->body;
				if (obj->body.otype == lm->urid.p_Set)
				{
					// Get the property the set message is setting
					const LV2_Atom* property = NULL;
					lv2_atom_object_get(obj, lm->urid.p_property, &property, 0);
					if (property && property->type == lm->urid.a_urid)
					{
						const uint32_t key = ((const LV2_Atom_URID*)property)->body;
						if (key == lm->urid.filetype_instr)
						{
							// a new file! pass the atom to the worker thread to load it
							if(lm->scheduler)
							{
								const uint32_t atom_size = (uint32_t)lv2_atom_total_size(&event->body);
								if (!lm->work_pending && atom_size > 0 && atom_size <= sizeof(lm->work_buf))
								{
									memcpy(lm->work_buf, &event->body, atom_size);
									lm->work_size = atom_size;
									lm->work_pending = 1;
									lm->scheduler->schedule_work(lm->scheduler->handle, atom_size, lm->work_buf);
								}
							}
#if(0)
							const LV2_Atom* file_path;
							lv2_atom_object_get(&event->body, lm->urid.p_value, &file_path, 0);
							if (file_path && file_path->type == lm->urid.a_path)
							{
								// Load file.
								char* path = (char*)LV2_ATOM_BODY_CONST(file_path);
								strcpy(lm->filepath,path);
							}
							//issue with this is if the file doesn't work, then we loose the old file path
#endif
						}//property is rvb file
					}//property is URID
				}
				else if (obj->body.otype == lm->urid.p_Get)
				{
					// Received a get message, emit our state (probably to UI)
					lv2_atom_forge_frame_time(&lm->forge, event->time.frames );//use current event's time
					LV2_Atom_Forge_Frame frame;
					lv2_atom_forge_object( &lm->forge, &frame, 0, lm->urid.p_Set);

					lv2_atom_forge_key(&lm->forge, lm->urid.p_property);
					lv2_atom_forge_urid(&lm->forge, lm->urid.filetype_instr);
					lv2_atom_forge_key(&lm->forge, lm->urid.p_value);
					lv2_atom_forge_path(&lm->forge, lm->filepath, strlen(lm->filepath)+1);

					lv2_atom_forge_pop(&lm->forge, &frame);

                    // Also send per-channel state snapshot for UI.
                    notify_channel_states(lm, midi, 1);
				}
     		}
     	}//if event not null
    }//for each atom

    // Emit per-channel state changes (effective bank/program) for the GUI/host.
    notify_channel_states(lm, midi, 0);
}

void* lv2_init_seq(const LV2_Feature * const* host_features)
{
    struct lmidi* lm = (struct lmidi*)calloc(1, sizeof(struct lmidi));
	for(int ch = 0; ch < 16; ++ch)
	{
		lm->chan_program_override[ch] = NULL;
        lm->last_bank_id[ch] = 0xFF;
        lm->last_program[ch] = 0xFF;
        lm->last_bank_msb[ch] = 0xFF;
        lm->last_bank_lsb[ch] = 0xFF;
	}
    if (!host_features)
    {
        fprintf(stderr, "reMID.lv2: missing host features (URID map required)\n");
        strcpy(lm->newfilepath,"");
        return (void*)lm;
    }
    for (int i = 0; host_features[i]; i++)
    {
        if (strcmp(host_features[i]->URI, LV2_URID__map) == 0)
        {
            LV2_URID_Map *urid_map = (LV2_URID_Map *) host_features[i]->data;
            if (urid_map)
            {
                lm->urid.m_midi_event = urid_map->map(urid_map->handle, LV2_MIDI__MidiEvent);
                lm->urid.a_blank = urid_map->map(urid_map->handle, LV2_ATOM__Blank);
                lm->urid.a_long = urid_map->map(urid_map->handle, LV2_ATOM__Long);
                lm->urid.a_float = urid_map->map(urid_map->handle, LV2_ATOM__Float);
                lm->urid.a_object = urid_map->map(urid_map->handle, LV2_ATOM__Object);
                lm->urid.a_path = urid_map->map(urid_map->handle, LV2_ATOM__Path);
                lm->urid.a_urid = urid_map->map(urid_map->handle, LV2_ATOM__URID);
                lm->urid.t_time = urid_map->map(urid_map->handle, LV2_TIME__Position);
                lm->urid.t_beatsperbar = urid_map->map(urid_map->handle, LV2_TIME__barBeat);
                lm->urid.t_bpm = urid_map->map(urid_map->handle, LV2_TIME__beatsPerMinute);
                lm->urid.t_speed = urid_map->map(urid_map->handle, LV2_TIME__speed);
                lm->urid.t_frame = urid_map->map(urid_map->handle, LV2_TIME__frame);
                lm->urid.t_framespersec = urid_map->map(urid_map->handle, LV2_TIME__framesPerSecond);
                lm->urid.p_Set = urid_map->map(urid_map->handle,LV2_PATCH__Set);
                lm->urid.p_Get = urid_map->map(urid_map->handle,LV2_PATCH__Get);
                lm->urid.p_property = urid_map->map(urid_map->handle,LV2_PATCH__property);
                lm->urid.p_value = urid_map->map(urid_map->handle,LV2_PATCH__value);
                lm->urid.filetype_instr = urid_map->map(urid_map->handle,INSTRUMENT_FILE_URI);
                lm->urid.remid_channel_state = urid_map->map(urid_map->handle, REMID_CHANNEL_STATE_URI);
                lm->urid.remid_channel = urid_map->map(urid_map->handle, REMID_CHANNEL_URI);
                lm->urid.remid_bank = urid_map->map(urid_map->handle, REMID_BANK_URI);
                lm->urid.remid_program = urid_map->map(urid_map->handle, REMID_PROGRAM_URI);
                lm->urid.remid_bank_msb = urid_map->map(urid_map->handle, REMID_BANK_MSB_URI);
                lm->urid.remid_bank_lsb = urid_map->map(urid_map->handle, REMID_BANK_LSB_URI);
                lm->urid.remid_bank_name = urid_map->map(urid_map->handle, REMID_BANK_NAME_URI);
                lm->urid.remid_patch_name = urid_map->map(urid_map->handle, REMID_PATCH_NAME_URI);
                lm->urid.polyphony = urid_map->map(urid_map->handle,POLYPHONY_URI);
                lm->urid.chiptype = urid_map->map(urid_map->handle,CHIPTYPE_URI);
                lm->urid.use_sid_vol = urid_map->map(urid_map->handle,USE_SID_VOL_URI);
                lv2_atom_forge_init(&lm->forge,urid_map);
            }
        }
        else if(strcmp(host_features[i]->URI,LV2_WORKER__schedule) == 0)
        {
            lm->scheduler = (LV2_Worker_Schedule*)host_features[i]->data;
        }
    }
    //strcpy(lm->filepath,"instruments.conf");//default path is in bundle
    strcpy(lm->newfilepath,"");//not loading anything
    return (void*)lm;
}

void lv2_close_seq(void* mseq)
{
    struct lmidi* lm = (struct lmidi*)mseq;
    free(lm);
}
