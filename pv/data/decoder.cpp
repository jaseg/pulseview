/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <libsigrokdecode/libsigrokdecode.h>

#include <boost/thread/thread.hpp>

#include "decoder.h"

#include <pv/data/logic.h>
#include <pv/data/logicsnapshot.h>
#include <pv/view/logicsignal.h>
#include <pv/view/decode/annotation.h>

using namespace boost;
using namespace std;

namespace pv {
namespace data {

const double Decoder::DecodeMargin = 1.0;
const double Decoder::DecodeThreshold = 0.2;
const int64_t Decoder::DecodeChunkLength = 4096;

Decoder::Decoder(const srd_decoder *const dec,
	std::map<const srd_probe*,
		boost::shared_ptr<pv::view::Signal> > probes) :
	_decoder(dec),
	_probes(probes),
	_decoder_inst(NULL)
{
	init_decoder();
	begin_decode();
}

Decoder::~Decoder()
{
	_decode_thread.interrupt();
	_decode_thread.join();
}

const srd_decoder* Decoder::get_decoder() const
{
	return _decoder;
}

const vector< shared_ptr<view::decode::Annotation> >
	Decoder::annotations() const
{
	lock_guard<mutex> lock(_annotations_mutex);
	return _annotations;
}

void Decoder::begin_decode()
{
	_decode_thread.interrupt();
	_decode_thread.join();

	if (_probes.empty())
		return;

	// We get the logic data of the first probe in the list.
	// This works because we are currently assuming all
	// LogicSignals have the same data/snapshot
	shared_ptr<pv::view::Signal> sig = (*_probes.begin()).second;
	assert(sig);
	const pv::view::LogicSignal *const l =
		dynamic_cast<pv::view::LogicSignal*>(sig.get());
	assert(l);
	shared_ptr<data::Logic> data = l->data();

	_decode_thread = boost::thread(&Decoder::decode_proc, this,
		data);
}

void Decoder::clear_snapshots()
{
}

void Decoder::init_decoder()
{
	if (!_probes.empty())
	{
		shared_ptr<pv::view::LogicSignal> logic_signal =
			dynamic_pointer_cast<pv::view::LogicSignal>(
				(*_probes.begin()).second);
		if (logic_signal) {
			shared_ptr<pv::data::Logic> data(
				logic_signal->data());
			if (data) {
				_samplerate = data->get_samplerate();
				_start_time = data->get_start_time();
			}
		}
	}

	_decoder_inst = srd_inst_new(_decoder->id, NULL);
	assert(_decoder_inst);

	_decoder_inst->data_samplerate = _samplerate;

	GHashTable *probes = g_hash_table_new_full(g_str_hash,
		g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

	for(map<const srd_probe*, shared_ptr<view::Signal> >::
		const_iterator i = _probes.begin();
		i != _probes.end(); i++)
	{
		shared_ptr<view::Signal> signal((*i).second);
		GVariant *const gvar = g_variant_new_int32(
			signal->probe()->index);
		g_variant_ref_sink(gvar);
		g_hash_table_insert(probes, (*i).first->id, gvar);
	}

	srd_inst_probe_set_all(_decoder_inst, probes);
}

void Decoder::decode_proc(shared_ptr<data::Logic> data)
{
	uint8_t chunk[DecodeChunkLength];

	assert(data);

	_annotations.clear();

	const deque< shared_ptr<pv::data::LogicSnapshot> > &snapshots =
		data->get_snapshots();
	if (snapshots.empty())
		return;

	const shared_ptr<pv::data::LogicSnapshot> &snapshot =
		snapshots.front();
	const int64_t sample_count = snapshot->get_sample_count() - 1;
	double samplerate = data->get_samplerate();

	// Show sample rate as 1Hz when it is unknown
	if (samplerate == 0.0)
		samplerate = 1.0;

	srd_session_start(_probes.size(), snapshot->unit_size(), samplerate);

	srd_pd_output_callback_add(SRD_OUTPUT_ANN,
		Decoder::annotation_callback, this);

	for (int64_t i = 0;
		!this_thread::interruption_requested() && i < sample_count;
		i += DecodeChunkLength)
	{
		const int64_t chunk_end = min(
			i + DecodeChunkLength, sample_count);
		snapshot->get_samples(chunk, i, chunk_end);

		if (srd_session_send(i, chunk, chunk_end - i) != SRD_OK)
			break;
	}
}

void Decoder::annotation_callback(srd_proto_data *pdata, void *decoder)
{
	using namespace pv::view::decode;

	assert(pdata);
	assert(decoder);

	Decoder *const d = (Decoder*)decoder;

	shared_ptr<Annotation> a(new Annotation(pdata));
	lock_guard<mutex> lock(d->_annotations_mutex);
	d->_annotations.push_back(a);
}

} // namespace data
} // namespace pv
