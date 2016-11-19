/*
#-------------------------------------------------------------------------------
# Copyright (c) 2014 Martin Marinov. Impl by Lucas Teske (2016)
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the GNU Public License v3.0
# which accompanies this distribution, and is available at
# http://www.gnu.org/licenses/gpl.html
#
# Contributors:
#     Martin Marinov - initial API and implementation
#-------------------------------------------------------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rtl-sdr.h>

#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>

#include <iostream>
#include <complex>

#include "TSDRPlugin.h"

#include "TSDRCodes.h"

#include <stdint.h>
#include <boost/algorithm/string.hpp>

#include "errors.hpp"

#define HOW_OFTEN_TO_CALL_CALLBACK_SEC (0.06)
#define FRACT_DROPPED_TO_TOLERATE (0)

rtlsdr_dev_t *dev;
namespace po = boost::program_options;

uint32_t req_freq = 105e6;
int req_gain = 1;
uint32_t req_rate = 2.56e6;
volatile int is_running = 0;

EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_getName(char * name) {
	strcpy(name, "TSDR RTLSDR Compatible Plugin");
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_init(const char * params) {
	// simulate argv and argc
	std::string sparams(params);

	typedef std::vector< std::string > split_vector_type;

	split_vector_type argscounter;
	boost::split( argscounter, sparams, boost::is_any_of(" "), boost::token_compress_on );

	const int argc = argscounter.size()+1;
	char ** argv = (char **) malloc(argc*sizeof(char *));
	char zerothtarg[] = "TSDRPlugin_RTLSDR";
	argv[0] = (char *) zerothtarg;
	for (int i = 0; i < argc-1; i++)
		argv[i+1] = (char *) argscounter[i].c_str();

	//variables to be set by po
	std::string args, file, ant,ref, tsrc;
	uint32_t bw;
	uint32_t index;
	//setup the program options
	po::options_description desc("Allowed options");
	desc.add_options()
			("rate", po::value<uint32_t>(&req_rate)->default_value(req_rate), "rate of incoming samples")
			("index", po::value<uint32_t>(&index)->default_value(0), "device index")
			("bw", po::value<uint32_t>(&bw), "IF filter bandwidth in Hz");

	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	} catch (std::exception const&  ex)
	{
		std::string msg(boost::str(boost::format("Error: %s\n\nTSDRPlugin_RTLSDR %s") % ex.what() % desc));
		RETURN_EXCEPTION(msg.c_str(), TSDR_PLUGIN_PARAMETERS_WRONG);
	}

	try {
		if (!rtlsdr_open(&dev, index)) { // TODO: Real index
			free(argv);
			RETURN_EXCEPTION("Cannot open device.", TSDR_CANNOT_OPEN_DEVICE);
		}

		if (rtlsdr_set_sample_rate(dev, req_rate) == -EINVAL) {
			free(argv);
			RETURN_EXCEPTION("Invalid sample rate.", TSDR_CANNOT_OPEN_DEVICE);
		}

		if (rtlsdr_set_center_freq(dev, req_freq) == 0) {
			free(argv);
			RETURN_EXCEPTION("Cannot set frequency.", TSDR_CANNOT_OPEN_DEVICE);
		}

		if (rtlsdr_set_tuner_gain(dev, req_gain)) {
			free(argv);
			RETURN_EXCEPTION("Cannot set tuner gain.", TSDR_CANNOT_OPEN_DEVICE);
		}

		//set the IF filter bandwidth
		if (vm.count("bw")) {
			rtlsdr_set_tuner_bandwidth(dev, bw );
		}

		boost::this_thread::sleep(boost::posix_time::seconds(1)); //allow for some setup time

	} catch (std::exception const&  ex)
	{
		free(argv);
		RETURN_EXCEPTION(ex.what(), TSDR_CANNOT_OPEN_DEVICE);
	}

	free(argv);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_setsamplerate(uint32_t rate) {
	if (is_running)
		return tsdrplugin_getsamplerate();

	req_rate = rate;

	rtlsdr_set_sample_rate(dev, req_rate);

	return req_rate;
}

EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_getsamplerate() {

	req_rate = rtlsdr_get_sample_rate(dev);

	return req_rate;
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setbasefreq(uint32_t freq) {
	req_freq = freq;

	rtlsdr_set_center_freq(dev, req_freq);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_stop(void) {
	is_running = 0;
	rtlsdr_cancel_async(dev);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setgain(float gain) {
	req_gain = gain;
	rtlsdr_set_tuner_gain(dev, req_gain);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

tsdrplugin_readasync_function *currentCb;

float *fBuff = NULL;

void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)	{
	for (int i=0; i<len; i++) {
		fBuff[i] = (buf[i] / 127.f) - 127.f;
	}

	(*currentCb)(fBuff, len, ctx, 0);
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_readasync(tsdrplugin_readasync_function cb, void *ctx) {
	is_running = 1;

	if (fBuff != NULL) {
		free(fBuff);
	}
	size_t buff_size = HOW_OFTEN_TO_CALL_CALLBACK_SEC * req_rate * 2;
	fBuff = (float *) malloc(sizeof(float) * buff_size);

	rtlsdr_read_async(dev, rtlsdr_callback, ctx, 0, buff_size);

	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_cleanup(void) {

	rtlsdr_close(dev);

		if (fBuff != NULL) {
			free(fBuff);
			fBuff = NULL;
		}

	is_running = 0;
}
