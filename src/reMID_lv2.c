
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>


#include "lv2_audio.h"
#include "lv2_midi.h"

#define REMID_URI "http://github.com/ssj71/reMID.lv2"



typedef struct arugalatastesbad
{
	float* L;
	float* R;
	LV2_Atom_Sequence* in;
	LV2_Atom_Sequence* out;

	void* everything;
}Remid;

static void apply_chan_program_overrides(struct super* s)
{
	// Apply before reading MIDI so incoming notes use the intended program.
	for(int ch = 0; ch < 16; ++ch)
	{
		const float* port = s->chan_program_override[ch];
		if(!port) continue;

		const int override_program_1based = (int)lrintf(*port);
		if(override_program_1based <= 0) continue;

		int program = override_program_1based - 1;
		if(program < 0) program = 0;
		if(program > 127) program = 127;

		s->midi->midi_channels[ch].in_use = 1;
		s->midi->midi_channels[ch].program = program;
	}
}

static void set_bundle_path(struct super* s, const char* bundle_path)
{
	if(!s)
		return;

	s->bundle_path[0] = 0;
	if(!bundle_path)
		return;

	snprintf(s->bundle_path, sizeof(s->bundle_path), "%s", bundle_path);

	const size_t n = strlen(s->bundle_path);
	if(n && s->bundle_path[n - 1] != '/')
	{
		if(n + 1 < sizeof(s->bundle_path))
		{
			s->bundle_path[n] = '/';
			s->bundle_path[n + 1] = 0;
		}
	}
}

static void resolve_bank_path(const struct super* s, const char* in_path, char* out_path, size_t out_len)
{
	if(!out_path || out_len == 0)
		return;

	out_path[0] = 0;
	if(!in_path)
		return;

	if(in_path[0] == '/')
	{
		snprintf(out_path, out_len, "%s", in_path);
		return;
	}

	if(s && s->bundle_path[0])
	{
		snprintf(out_path, out_len, "%s%s", s->bundle_path, in_path);
		return;
	}

	snprintf(out_path, out_len, "%s", in_path);
}

LV2_Handle init_remid(const LV2_Descriptor *descriptor,double sample_freq, const char *bundle_path,const LV2_Feature * const* host_features)
{
	char instr_file[512];
	char base[512];
	snprintf(base, sizeof(base), "%s", bundle_path ? bundle_path : "");
	const size_t n = strlen(base);
	if(n && base[n - 1] != '/' && n + 1 < sizeof(base))
	{
		base[n] = '/';
		base[n + 1] = 0;
	}
	snprintf(instr_file, sizeof(instr_file), "%sinstruments/banks/bank-all-0.swibank", base);

	struct super* s = init_lv2_audio(lrint(sample_freq), instr_file, host_features);
	set_bundle_path(s, base);
	struct lmidi* lm = (struct lmidi*)s->midi->seq;
	snprintf(lm->filepath, sizeof(lm->filepath), "%s", instr_file);
	for(int ch = 0; ch < 16; ++ch)
	{
		s->chan_program_override[ch] = NULL;
		lm->chan_program_override[ch] = NULL;
	}
	return (void*)s;
}

void connect_remid_ports(LV2_Handle handle, uint32_t port, void* data)
{
	struct super* s = (struct super*)handle;
	struct lmidi* lm = (struct lmidi*)s->midi->seq;
	switch(port)
	{
	case 0:
		s->outl = (float*)data;
		break;
	case 1:
		s->outr = (float*)data;
		break;
	case 2:
		lm->atom_in_p = (LV2_Atom_Sequence*)data;
		break;
	case 3:
		lm->atom_out_p = (LV2_Atom_Sequence*)data;
		break;
	default:
		if(port >= 4 && port < 20)
		{
			const int ch = (int)port - 4;
			s->chan_program_override[ch] = (const float*)data;
			lm->chan_program_override[ch] = (const float*)data;
		}
		break;
	}
}

void run_remid(LV2_Handle handle, uint32_t nframes)
{
	apply_chan_program_overrides((struct super*)handle);
	process(nframes,handle);
}

void cleanup_remid(LV2_Handle handle)
{
	cleanup_audio(handle);
}

//this is done in a separate thread;
static LV2_Worker_Status remidwork(LV2_Handle handle, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rhandle, uint32_t size, const void* data)
{
	struct super* s = (struct super*)handle;
	struct lmidi* lm = s->midi->seq;
    LV2_Atom_Object* obj = (LV2_Atom_Object*)data;
    const LV2_Atom* file_path = NULL;

    //if we're here, then we will need a new file, get rid of old ones
    while(s->newmidi){usleep(1000);}//wait if in the middle of switching files
    if(s->oldmidi)
    	free(s->oldmidi);
    if(s->old_bank)
    	sw_bank_free(s->old_bank);
    s->oldmidi = 0;
    s->old_bank = 0;

    //work was scheduled to load a new file
    lv2_atom_object_get(obj, lm->urid.p_value, &file_path, 0);
    if (file_path && file_path->type == lm->urid.a_path)
    {
        // Load file.
        const char* path = (const char*)LV2_ATOM_BODY_CONST(file_path);
        char resolved[512];
        resolve_bank_path(s, path, resolved, sizeof(resolved));
        snprintf(lm->newfilepath, sizeof(lm->newfilepath), "%s", resolved);

        //need to create new arrays based on this instrument file
        s->newmidi = new_midi_arrays(s->midi,s->sid_bank->polyphony);
        s->new_bank = sw_bank_load(resolved);
        if(!s->new_bank)
        {
        	free(s->newmidi);
        	s->newmidi = 0;
        	return LV2_WORKER_ERR_UNKNOWN;
        }

        respond(rhandle,0,0);//not passing the new arrays directly, using plugin newmidi etc pointers
    }//got file
    else
        return LV2_WORKER_ERR_UNKNOWN;

    return LV2_WORKER_SUCCESS;

}
//this one is run in RT thread
static LV2_Worker_Status remidwork_response(LV2_Handle handle, uint32_t size, const void* data)
{
	//just switch to the new instrument arrays
	struct super* s = (struct super*)handle;
	struct lmidi* lm = s->midi->seq;
	s->oldmidi = s->midi;
	s->old_bank = s->bank;
	s->midi = s->newmidi;
	s->bank = s->new_bank;
	s->newmidi = 0;
	s->new_bank = 0;
	strcpy(lm->filepath,lm->newfilepath);
	lm->newfilepath[0] = 1; //this special case to notify host
    return LV2_WORKER_SUCCESS;
}
//this is not RT
static LV2_State_Status remidsave(LV2_Handle handle, LV2_State_Store_Function  store, LV2_State_Handle state_handle,
		uint32_t flags, const LV2_Feature* const* features)
{
	struct super* s = (struct super*)handle;
	struct lmidi* lm = s->midi->seq;

    LV2_State_Map_Path* map_path = NULL;
    for (int i = 0; features[i]; ++i)
    {
        if (!strcmp(features[i]->URI, LV2_STATE__mapPath))
        {
            map_path = (LV2_State_Map_Path*)features[i]->data;
        }
    }

    const char* to_store = lm->filepath;
    uint32_t store_flags = LV2_STATE_IS_POD;
    char* abstractpath = NULL;
    if(map_path)
    {
    	abstractpath = map_path->abstract_path(map_path->handle, lm->filepath);
    	if(abstractpath)
    	{
    		to_store = abstractpath;
    		store_flags |= LV2_STATE_IS_PORTABLE;
    	}
    }
    else if (s && s->bundle_path[0])
    {
    	// If mapPath is unavailable, prefer storing paths relative to the bundle root.
    	// MODEP warns when plugins write absolute paths into patch/state.
    	const size_t bl = strlen(s->bundle_path);
    	if (!strncmp(lm->filepath, s->bundle_path, bl))
    	{
    		to_store = lm->filepath + bl;
    	}
    }

    store(state_handle, lm->urid.filetype_instr, to_store, strlen(to_store) + 1,
    		lm->urid.a_path, store_flags);

    if(abstractpath) free(abstractpath);

    return LV2_STATE_SUCCESS;

}
//this is not RT
static LV2_State_Status remidrestore(LV2_Handle handle, LV2_State_Retrieve_Function retrieve,
		LV2_State_Handle state_handle, uint32_t flags, const LV2_Feature* const* features)
{
	struct super* s = (struct super*)handle;
	struct lmidi* lm = s->midi->seq;
    size_t   size;
    uint32_t type;
    uint32_t valflags;
    const char* path = 0;
    LV2_State_Map_Path* map_path = NULL;

    for (int i = 0; features[i]; ++i)
    {
        if (!strcmp(features[i]->URI, LV2_STATE__mapPath))
        {
            map_path = (LV2_State_Map_Path*)features[i]->data;
        }
    }

    const void* value = retrieve( state_handle, lm->urid.filetype_instr, &size, &type, &valflags);
    if (value)
		path = (const char*)value;

    if(path)
    {
    	const char* load_path = path;
    	char* abs_path = NULL;
		char resolved[512];

    	if((valflags & LV2_STATE_IS_PORTABLE) && map_path)
    	{
    		abs_path = map_path->absolute_path(map_path->handle, path);
    		if(abs_path) load_path = abs_path;
    	}
    	else if(path[0] != '/')
    	{
    		resolve_bank_path(s, path, resolved, sizeof(resolved));
    		load_path = resolved;
    	}

    	sw_bank_t* loaded = sw_bank_load(load_path);
    	if(!loaded)
    	{
    		if(abs_path) free(abs_path);
    		return LV2_STATE_ERR_UNKNOWN;
    	}

		if(s->oldmidi)
			free(s->oldmidi);
		if(s->old_bank)
			sw_bank_free(s->old_bank);
		s->oldmidi = 0;
		s->old_bank = 0;
		if(s->newmidi && s->newmidi != s->midi)
		{
			//was loading a file, but this will supersede
			free(s->newmidi);
			sw_bank_free(s->new_bank);
		}
		s->newmidi = 0;
		s->new_bank = 0;

		s->oldmidi = s->midi;
		s->old_bank = s->bank;

        s->midi = new_midi_arrays(s->oldmidi,s->sid_bank->polyphony);
        s->bank = loaded;
        free(s->oldmidi);
		sw_bank_free(s->old_bank);
		s->oldmidi = 0;
		s->old_bank = 0;
		snprintf(lm->filepath, sizeof(lm->filepath), "%s", load_path);
		lm->newfilepath[0] = 1;

		if(abs_path) free(abs_path);
    }

    return LV2_STATE_SUCCESS;

}

static const void* remid_extension_data(const char* uri)
{
    static const LV2_Worker_Interface worker = { remidwork, remidwork_response, NULL };
    static const LV2_State_Interface state_iface = { remidsave, remidrestore };
    if (!strcmp(uri, LV2_STATE__interface))
    {
        return &state_iface;
    }
    else if (!strcmp(uri, LV2_WORKER__interface))
    {
        return &worker;
    }
    return NULL;
}

static const LV2_Descriptor remid_lv2_descriptor=
{
    REMID_URI,
    init_remid,
    connect_remid_ports,
    0,//activate
    run_remid,
    0,//deactivate
    cleanup_remid,
	remid_extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    switch (index)
    {
    case 0:
        return &remid_lv2_descriptor ;
    }
    return 0;
}
