/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#if defined(HAVE_LIBPCAP) && defined(HAVE_LIBAIRPCAP) && defined(SYS_CYGWIN)

#include "packetsource_airpcap.h"

#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "packetsourcetracker.h"

// Work around broken pcap.h on cygwin... this is a TERRIBLE THING TO DO but
// libwpcap on the airpcap cd seems to come with a pcap.h header for standard
// pcap, while the lib contains this symbol.
#if defined(HAVE_PCAP_GETEVENT)
int pcap_event(pcap_t *);
#endif

// Prototypes of Windows-specific pcap functions.
// wpcap.dll contains these functions, but they are not exported to cygwin because
// cygwin doesn't "officially" support the Windows extensions. These functions, 
// however, are safe to use.
extern "C" PAirpcapHandle pcap_get_airpcap_handle(pcap_t *p);
extern "C" HANDLE pcap_getevent (pcap_t *p);
extern "C" int pcap_setmintocopy (pcap_t *p, int size);

int PacketSource_AirPcap::OpenSource() {
	char errstr[STATUS_MAX] = "";
	channel = 0;
	char *unconst = strdup(interface.c_str());

	pd = pcap_open_live(unconst, MAX_PACKET_LEN, 1, 1000, errstr);

	free(unconst);

	if (strlen(errstr) > 0) {
		_MSG(errstr, MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return -1;
	}

	paused = 0;
	errstr[0] = '\0';
	num_packets = 0;

	if (DatalinkType() < 0) {
		pcap_close(pd);
		return -1;
	}

	// Fetch the airpcap handle
	if ((airpcap_handle = pcap_get_airpcap_handle(pd)) == NULL) {
		_MSG("Adapter " + interface + " does not have airpcap wireless extensions",
			 MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		pcap_close(pd);
		return -1;
	}

	// Set the link mode to give us radiotap headers
	// Non-deterministic, we don't know if we've set radiotap mode or just
	// changed the link type, removed
#if 0
	if (!AirpcapSetLinkType(airpcap_handle, AIRPCAP_LT_802_11_PLUS_RADIO)) {
		_MSG("Adapter " + interface + " failed setting airpcap radiotap "
			 "link layer: " + 
			 string((const char *) AirpcapGetLastError(airpcap_handle)),
			 MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		pcap_close(pd);
		return -1;
	}
#endif

	// Tell the adapter to only give us packets which pass internal FCS validation.
	// All we do is throw away frames which do not, so theres no reason to add
	// the overhead of locally processing the checksum.
	if (!AirpcapSetFcsValidation(airpcap_handle, AIRPCAP_VT_ACCEPT_CORRECT_FRAMES)) {
		_MSG("Airpcap adapter " + interface + " failed setting FCS validation: " +
			 string((const char *) AirpcapGetLastError(airpcap_handle)), 
			 MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		pcap_close(pd);
		return -1;
	}

	// Add it to the Handle to FD mangler
	fd_mangle.AddHandle(pcap_getevent(pd));
	fd_mangle.Activate();

	return 0;
}

int PacketSource_AirPcap::Poll() {
	int ret;

	if ((ret = PacketSource_Pcap::Poll()) == 0) {
		fd_mangle.Reset();
		fd_mangle.Signalread();
	}

	return ret;
}

int PacketSource_AirPcap::AutotypeProbe(string in_device) {
	return 0;
}

int PacketSource_AirPcap::RegisterSources(Packetsourcetracker *tracker) {
	tracker->RegisterPacketsource("airpcap", this, 0, "IEEE80211b", 6);
	tracker->RegisterPacketsource("airpcap_ask", this, 0, "IEEE80211b", 6);
	return 1;
}

PacketSource_AirPcap::PacketSource_AirPcap(GlobalRegistry *in_globalreg, 
										   string in_type, string in_name,
										   string in_dev, string in_opts): 
	PacketSource_Pcap(in_globalreg, in_type, in_name, in_dev, in_opts) {

	// Go through the prompting game for 'ask' variant
	if (in_type == "airpcap_ask") {
		pcap_if_t *alldevs, *d;
		int i, intnum;
		char errbuf[1024];

		if (pcap_findalldevs(&alldevs, errbuf) == -1) {
			_MSG("AirPcapSource failed to find pcap devices: " + string(errbuf),
				 MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
			return;
		}

		fprintf(stdout, "Available interfaces:\n");
		for (d = alldevs, i = 0; d != NULL; d = d->next) {
			fprintf(stdout, "%d.  %s\n", ++i, d->name);
			if (d->description)
				fprintf(stdout, "   %s\n", d->description);
			else
				fprintf(stdout, "   No description available\n");
		}

		if (i == 0) {
			pcap_freealldevs(alldevs);
			_MSG("airPcapSource failed to find any devices, are WinPcap and AirPcap "
				 "properly installed?", MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
			return;
		}

		while (1) {
			fprintf(stdout, "Enter interface number (1-%d): ", i);
			if (fscanf(stdin, "%d", &intnum) != 1) {
				fprintf(stdout, "Invalid entry, expected a number\n");
				continue;
			}

			if (intnum < 1 || intnum > i) {
				fprintf(stdout, "Invalid entry, expected between 1 and %d\n", i);
				continue;
			}

			break;
		}

		// Iterate
		for (d = alldevs, i = 0; i < intnum - 1; d = d->next, i++)
			;

		interface = string(d->name);

		pcap_freealldevs(alldevs);
	}

}

int PacketSource_AirPcap::EnableMonitor() {
	// This is handled during the open because we need a working pcap handle
	return 1;
}

int PacketSource_AirPcap::DisableMonitor() {
	return PACKSOURCE_UNMONITOR_RET_CANTUNMON;
}

int PacketSource_AirPcap::FetchHardwareChannel() {
	unsigned int ch;
	if (!AirpcapGetDeviceChannel(airpcap_handle, &ch))
		return -1;

	return (int) ch;
}

int PacketSource_AirPcap::FetchDescriptor() {
	return fd_mangle.GetFd();
}

int PacketSource_AirPcap::SetChannel(unsigned int in_ch) {
	if (!AirpcapSetDeviceChannel(airpcap_handle, in_ch)) {
		_MSG("Airpcap adapter " + interface + " failed setting channel: " +
			 string((const char *) AirpcapGetLastError(airpcap_handle)), 
			 MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return -1;
	}

	return 0;
}

#endif 

