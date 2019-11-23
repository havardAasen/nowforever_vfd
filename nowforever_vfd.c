/*
    nowforever_vfd.c

    This is a userspace program that intefaces the Nowforever D100/E100 VFD
    to the LinuxCNC HAL, using RS485 ModBus RTU.
*/

#include <stdio.h>
#include <unistd.h>

#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include <getopt.h>

static int done;
char *modname = "nowforever_vfd";

// Command-line options
static struct option long_options[] = {
    {"device", 1, 0, 'd'},
    {"debug", 0, 0, 'g'},
    {"help", 0, 0, 'h'},
    {"name", 1, 0 , 'n'},
    {"parity", 1, 0, 'p'},
    {"rate", 1, 0, 'r'},
    {"stopbits", 1, 0, 's'},
    {"target", 1, 0, 't'},
    {"verbose", 0, 0, 'v'},
    {"disable", no_argument, NULL, 'X'},
    {0,0,0,0}
};

static char *option_string = "gb:d:hn:p:r:s:t:vA:X";


// The old libmodbus (v2?) used strings to indicate parity, the new one
// (v3.0.1) uses chars.  The gs2_vfd driver gets the string indicating the
// parity to use from the command line, and I don't want to change the
// command-line usage.  The command-line argument string must match an
// entry in paritystrings, and the index of the matching string is used as
// the index to the parity character for the new libmodbus.
static char *paritystrings[] = {"even", "odd", "none", NULL};
static char paritychars[] = {'E', 'O', 'N'};

static char *ratestrings[] = {"2400", "4800", "9600", "19200", "38400", NULL};
static char *stopstrings[] = {"1", "2", NULL};

static void quit(int sig)
{
    done = 1;
}

int match_string(char *string, char **matches) {
    int len, which, match;
    which = 0;
    match = -1;
    if ((matches == NULL) || (string == NULL)) return -1;
    len = strlen(string);
    while (matches[which] != NULL) {
        if ((!strncmp(string, matches[which], len)) && (len <= strlen(matches[which])))
        {
            // Multiple matches
            if (match >= 0)
            {
                return -1;
            }
            match = which;
        }
        ++which;
    }
    return match;
}


void usage(int argc, char **argv)
{
    printf("Usage: %s [options]\n", argv[0]);
    printf(
    "This is a userspace HAL program, typically loaded using the halcmd \"loaduser\" command:\n"
    "   loadusr nowforever_vfd\n"
    "There are several command-line options. Options that have a set list of possible values may\n"
    "   be set by using any number of characters that are unique. For example, --rate 4 will use\n"
    "   a baud rate of 4800, since no other avaliable baud rates start with \"4\"\n"
    "-d or -device <path> (default: /dev/ttyUSB0)\n"
    "   Set the name of the serial device node to use\n"
    "-v or --verbose\n"
    "    Turn on verbose mode.\n"
    "-g or --debug\n"
    "    Turn on debug mode.  This will cause all modbus messages to be\n"
    "    printed in hex on the terminal.\n"
    "-n or --name <string> (default: nowforever_vfd)\n"
    "    Set the name of the HAL module.  The HAL comp name will be set to <string>, and all pin\n"
    "    and parameter names will begin with <string>.\n"
    "-p or --parity {even,odd,none} (default: none)\n"
    "    Set serial parity to even, odd, or none.\n"
    "-r or --rate <n> (default: 19200)\n"
    "   Set baud rate to <n>. It is an error if the rate is not one of the following:\n"
    "   2400, 4800, 9600, 19200, 38400\n"
    "-s or --stopbits {1,2} (default: 1)\n"
    "    Set serial stop bits to 1 or 2\n"
    "-t or --target <n> (default: 1)\n"
    "    Set MODBUS target (slave) number.  This must match the device number you set on the device.\n"
    "-X, --disable\n"
    "    Set this flag to disable the control by default (sets default value of 'enable' pin to 0)\n"
    );
}

int main(int argc, char **argv)
{
    int retval;
    int slave;
    int baud, bits, stopbits, verbose, debug;
    char *device, *endarg;
    char parity;
    int opt;
    int argindex, argvalue;
    int enabled;

    retval = 0;
    done = 0;

    // Assume that nothing is specified on the command line
    baud = 19200;
    bits = 8;
    stopbits = 1;
    debug = 0;
    verbose = 0;
    device = "/dev/ttyUSB0";
    parity = 'n';


    /* Slave register info */
    slave = 1;

    // Process command line options
    while ((opt=getopt_long(argc, argv, option_string, long_options, NULL)) != -1)
    {
        switch (opt)
        {
            // Disable by default on startup
            case 'X':
                enabled = 0;
                break;

            // Device name, default /dev/ttyUSB0
            case 'd':
                // Could check the device name here, but we'll leave it to the library open
                if (strlen(optarg) > FILENAME_MAX)
                {
                    printf("nowforever_vfd: ERROR: device node  name s to long: %s\n", optarg);
                    retval -1;
                    goto out_noclose;
                }
                device = strdup(optarg);
                break;

            case 'g':
                debug = 1;
                break;

            case 'v':
                verbose = 1;
                break;

            // Module base name
            case 'n':  
                if (strlen(optarg) > 20) 
                {  // TODO
                    printf("nowforever_vfd: ERROR: HAL module name to long: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }

            // Baud rate, defaults to 19200
            case 'r':  
                argindex = match_string(optarg, ratestrings);
                if (argindex<0)
                {
                    printf("nowforever_vfd: ERROR: invalid baud rate: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                baud = atoi(ratestrings[argindex]);
                break;

            // Stop bits, defaults to 1
            case 's':
                argindex = match_string(optarg, stopstrings);
                if (argindex < 0)
                {
                    printf("nowforever_vfvd: ERROR: invalid number of stop bits: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                stopbits = atoi(stopstrings[argindex]);
                break;
            
            // Target number (MODBUS ID), default 1
            case 't':
                argvalue = strtol(optarg, &endarg, 10);
                if ((*endarg != '\0') || (argvalue < 1) || (argvalue > 254))
                {
                    printf("nowforever_vfd: ERROR: invalid slave number: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                slave = argvalue;
                break;

            case 'h':
            default:
                usage(argc, argv);
                exit(0);
                break;
        }
    }

    printf("%s: device='%s', baud='%d', parity='%c', bits=%d, stopbits=%d, address=%d, enabled=%d\n",
            modname, device, baud, parity, bits, stopbits, slave, enabled);
    // Point TERM and INT signals at our quit function.
    // If a signal is received between here and the main loop, it should prevent
    // some initialization from happening.
    signal(SIGINT, quit);
    signal(SIGTERM, quit);

    // If we get here, then everythin is fine, so just clean up and exit
    retval = 0;

    out_noclose:
        return retval;
}