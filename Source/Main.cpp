/*
 * This file is part of loop4r_control.
 * Copyright (C) 2018 Atin Malaviya.  https://www.github.com/atinm
 *
 * loop4r_control is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * loop4r_control is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 ==============================================================================
 loop4r_control for controlling sooperlooper via an FCB1010 with the EurekaProm
 set to I/O mode. The goal is to allow control of sooperlooper via just the
 controller and have the LEDs etc reflect the state of sooperlooper.

 Forked from the https://github.com/gbevin/ReceiveMIDI and
 https://github.com/gbevin/SendMIDI source as a MIDI starting point.
 ==============================================================================
 */

#include "../JuceLibraryCode/JuceHeader.h"
#include <sstream>


//==============================================================================
enum CommandIndex
{
    NONE,
    LIST,
    FCB1010_IN,
    FCB1010_OUT,
    SL_OUT,
    VIRTUAL_OUT,
    CHANNEL,
    BASE_NOTE,
    OSC_IN,
    OSC_OUT
};

enum LoopStates
{
    Unknown = -1,
    Off = 0,
    WaitStart,
    Recording,
    WaitStop,
    Playing,
    Overdubbing,
    Multiplying,
    Inserting,
    Replacing,
    Delay,
    Muted,
    Scratching,
    OneShot,
    Substitute,
    Paused
};

enum LedStates
{
    Dark,
    Light,
    SlowBlink,
    Blink,
    FastBlink
};

static const String& DEFAULT_VIRTUAL_OUT_NAME = "loop4r_control_out";
static const int DEFAULT_BASE_NOTE = 64;
static const int NUM_LEDS = 23;
static const int UP = 10;
static const int DOWN = 11;

// timers
static const int TIMER_OFF = 0;
static const int TIMER_FASTBLINK = 1;
static const int TIMER_BLINK = 2;
static const int TIMER_SLOWBLINK = 3;

// pedals (0-3 are assigned to loops 1..4)
static const int RECORD = 4;
static const int MULTIPLY = 5;
static const int INSERT = 6;
static const int REPLACE = 7;
static const int SUBSTITUTE = 8;
static const int UNDO = 9;
static const int CLEAR = 10;
static const int MUTE = 11;

static const int MUTE_ON = 19;
static const int HEARTBEAT = 22;

struct ApplicationCommand
{
    static ApplicationCommand Dummy()
    {
	return {"", "", NONE, 0, "", ""};
    }

    void clear()
    {
	param_ = "";
	command_ = NONE;
	expectedOptions_ = 0;
	optionsDescription_ = "";
	commandDescription_ = "";
	opts_.clear();
    }

    String param_;
    String altParam_;
    CommandIndex command_;
    int expectedOptions_;
    String optionsDescription_;
    String commandDescription_;
    StringArray opts_;
};

struct LED {
    int index_;
    bool on_;
    int timer_;
    LedStates state_;

    void clear()
    {
	on_ = false;
	timer_ = TIMER_OFF;
	state_ = Dark;
    }
};

struct Loop {
    int index_;
    LoopStates state_;
    LED& led_;

    void clear()
    {
	// we don't clear index_
	state_ = Off;
	led_.clear();
    }
};

inline float sign(float value)
{
    return (float)(value > 0.) - (value < 0.);
}

class loop4r_controlApplication  : public JUCEApplicationBase, public MidiInputCallback,
public Timer, private OSCReceiver::Listener<OSCReceiver::MessageLoopCallback>
{
public:
    //==============================================================================
    loop4r_controlApplication()
    {
	commands_.add({"fin",   "fcb1010 midi in",  FCB1010_IN,         1, "name",           "Set the name of the FCB1010 MIDI input port"});
	commands_.add({"fout",  "fcb1010 midi out", FCB1010_OUT,        1, "name",           "Set the name of the FCB1010 MIDI output port"});
	commands_.add({"slout", "sooperlooper midi out", SL_OUT,        1, "name",           "Set the name of the SooperLooper MIDI input port"});
	commands_.add({"vout",  "virtual",          VIRTUAL_OUT,       -1, "(name)",         "Use virtual MIDI output port with optional name (Linux/macOS)"});
	commands_.add({"list",  "",                 LIST,               0, "",               "Lists the MIDI ports"});
	commands_.add({"ch",    "channel",          CHANNEL,            1, "number",         "Set MIDI channel for the commands (0-16), defaults to 0"});
	commands_.add({"base",  "base note",        BASE_NOTE,          1, "number",         "Starting note"});
	commands_.add({"oin",   "osc in",           OSC_IN,             1, "number",         "OSC receive port"});
	commands_.add({"oout",  "osc out",          OSC_OUT,            1, "number",         "OSC send port"});

	for (auto i=0; i<NUM_LEDS; i++)
	{
	    leds_.add({i, false, TIMER_OFF, Dark});
	}

	channel_ = 1;
	baseNote_ = DEFAULT_BASE_NOTE;
	selected_ = 0;
	noteNumbersOutput_ = false;
	useHexadecimalsByDefault_ = false;
	oscSendPort_ = 9951;
	oscReceivePort_ = 9000;
	loopCount_ = 0;
	hostUrl_ = "";
	version_ = "";
	selectedLoop_ = -1;
	pinged_ = false;
	mode_ = 0;
	heartbeat_ = 5;
	engineId_ = 0;
	currentCommand_ = ApplicationCommand::Dummy();
    }

    const String getApplicationName() override       { return ProjectInfo::projectName; }
    const String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override       { return false; }

    //==============================================================================
    void initialise (const String& commandLine) override
    {
	StringArray cmdLineParams(getCommandLineParameterArray());
	if (cmdLineParams.contains("--help") || cmdLineParams.contains("-h"))
	{
	    printUsage();
	    systemRequestedQuit();
	    return;
	}
	else if (cmdLineParams.contains("--version"))
	{
	    printVersion();
	    systemRequestedQuit();
	    return;
	}

	parseParameters(cmdLineParams);

	if (cmdLineParams.contains("--"))
	{
	    while (std::cin)
	    {
		std::string line;
		getline(std::cin, line);
		StringArray params = parseLineAsParameters(line);
		parseParameters(params);
	    }
	}

	if (cmdLineParams.isEmpty())
	{
	    printUsage();
	    systemRequestedQuit();
	}
	else
	{
	    startTimer(200);
	}
    }

    void timerCallback() override
    {
	if (fullMidiInName_.isNotEmpty() && !MidiInput::getDevices().contains(fullMidiInName_))
	{
	    std::cerr << "MIDI input port \"" << fullMidiInName_ << "\" got disconnected, waiting." << std::endl;

	    fullMidiInName_ = String();
	    midiIn_ = nullptr;
	}
	else if ((midiInName_.isNotEmpty() && midiIn_ == nullptr))
	{
	    if (tryToConnectMidiInput())
	    {
		std::cerr << "Connected to MIDI input port \"" << fullMidiInName_ << "\"." << std::endl;
	    }
	}

	if (midiOutName_.isNotEmpty() && midiOut_ == nullptr)
	{
	    int index = MidiOutput::getDevices().indexOf(midiOutName_);
	    if (index >= 0)
	    {
		midiOut_ = MidiOutput::openDevice(index);
	    }
	    else
	    {
		StringArray devices = MidiOutput::getDevices();
		for (int i = 0; i < devices.size(); ++i)
		{
		    if (devices[i].containsIgnoreCase(midiOutName_))
		    {
			midiOut_ = MidiOutput::openDevice(i);
			midiOutName_ = devices[i];
			break;
		    }
		}
	    }
	    if (midiOut_ == nullptr)
	    {
		std::cerr << "Couldn't find MIDI output port \"" << midiOutName_ << "\"" << std::endl;
	    }
	    else
	    {
		// initialize the leds to off
		for (auto i=0; i<NUM_LEDS; i++)
		    ledOff(i);
	    }
	}

	if (slMidiOutName_.isNotEmpty() && virtMidiOutName_.isEmpty() && slMidiOut_ == nullptr)
	{
	    int index = MidiOutput::getDevices().indexOf(slMidiOutName_);
	    if (index >= 0)
	    {
		slMidiOut_ = MidiOutput::openDevice(index);
	    }
	    else
	    {
		StringArray devices = MidiOutput::getDevices();
		for (int i = 0; i < devices.size(); ++i)
		{
		    if (devices[i].containsIgnoreCase(midiOutName_))
		    {
			slMidiOut_ = MidiOutput::openDevice(i);
			slMidiOutName_ = devices[i];
			break;
		    }
		}
	    }
	    if (slMidiOut_ == nullptr)
	    {
		std::cerr << "Couldn't find SooperLooper MIDI input port \"" << slMidiOutName_ << "\" to output to." << std::endl;
	    }
	}

#if (JUCE_LINUX || JUCE_MAC)
	if (virtMidiOutName_.isNotEmpty() && slMidiOutName_.isEmpty() && slMidiOut_ == nullptr)
	{
	    slMidiOut_ = MidiOutput::createNewDevice(virtMidiOutName_);
	    if (slMidiOut_ == nullptr)
	    {
		std::cerr << "Couldn't create virtual MIDI output port \"" << virtMidiOutName_ << "\"" << std::endl;
	    }
#else
	    std::cerr << "Virtual MIDI output ports are not supported on Windows" << std::endl;
	    exit;
#endif
	}

	if (currentReceivePort_ < 0 || currentSendPort_ < 0) {
	    if (tryToConnectOsc())
	    {
		std::cerr << "Connected to OSC ports " << (int)currentReceivePort_ << " (in) and " << (int) currentSendPort_ << " (out)" << std::endl;
		heartbeat_ = 5;
	    }
	}
	else
	{
	    // heartbeat
	    if (heartbeat_ == 0)
	    {
		oscSender.send("/ping", (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/", (String) "/heartbeat");
	    }
	    else if (heartbeat_ < -5) // give a second before we try reconnecting
	    {
		// we've lost heartbeat, try reconnecting
		currentReceivePort_ = -1;
		currentSendPort_ = -1;
		if (tryToConnectOsc())
		{
		    std::cerr << "Reconnected to OSC ports " << (int)currentReceivePort_ << " (in) and " << (int) currentSendPort_ << " (out)" << std::endl;
		    heartbeat_ = 5;
		}
	    }
	    else
	    {
		--heartbeat_;
	    }

	    // handle pedal led state for blinking pedals
	    for (auto&& loop : loops_)
	    {
		if (loop.led_.state_ == SlowBlink || loop.led_.state_ == Blink || loop.led_.state_ == FastBlink)
		{
		    if (loop.led_.timer_ <= 0)
		    {
			if (loop.led_.on_) {
			    ledOff(loop.index_);
			}
			else
			{
			    ledOn(loop.index_);
			}
			switch (loop.led_.state_) {
			    case SlowBlink:
				loop.led_.timer_ = TIMER_SLOWBLINK;
				break;
			    case Blink:
				loop.led_.timer_ = TIMER_BLINK;
				break;
			    case FastBlink:
				loop.led_.timer_ = TIMER_FASTBLINK;
				break;
			    default:
				loop.led_.timer_ = TIMER_BLINK;
				break;
			}
		    }
		    else
		    {
			loop.led_.timer_--;
		    }
		}
	    }
	}
    }

    void updateLoops()
    {
	for (auto&& loop : loops_)
	{
	    updateLoopLedState(loop, loop.state_);
	}
    }

    void updateLoopLedState(Loop& loop, LoopStates newState)
    {
	switch (newState)
	{
	    case Unknown:
	    case Off:
		loop.led_.state_ = Dark;
		ledOff(loop.index_);
		loop.led_.timer_ = TIMER_OFF;
		break;
	    case WaitStart:
	    case WaitStop:
		loop.led_.state_ = FastBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_FASTBLINK;
		break;
	    case Recording:
		loop.led_.state_ = SlowBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_SLOWBLINK;
		break;
	    case Overdubbing:
		loop.led_.state_ = SlowBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_SLOWBLINK;
		break;
	    case Inserting:
		loop.led_.state_ = FastBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_FASTBLINK;
		ledOn(INSERT);
		break;
	    case Replacing:
		loop.led_.state_ = FastBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_FASTBLINK;
		ledOn(REPLACE);
		break;
	    case Substitute:
		loop.led_.state_ = FastBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_FASTBLINK;
		ledOn(SUBSTITUTE);
		break;
	    case Multiplying:
		loop.led_.state_ = FastBlink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_FASTBLINK;
		ledOn(MULTIPLY);
		break;
	    case Delay:
		loop.led_.state_ = Light;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_OFF;
		break;
	    case Scratching:
		loop.led_.state_ = Light;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_OFF;
		break;
	    case OneShot:
		loop.led_.state_ = Light;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_OFF;
		break;
	    case Playing:
		loop.led_.state_ = Light;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_OFF;
		break;
	    case Muted:
	    case Paused:
		loop.led_.state_ = Blink;
		ledOn(loop.index_);
		loop.led_.timer_ = TIMER_BLINK;
		break;
	    default:
		loop.led_.state_ = Dark;
		ledOff(loop.index_);
		loop.led_.timer_ = TIMER_OFF;
		break;
	}

	if (newState != loop.state_)
	{
	    // turn off any leds that are no longer active
	    switch(loop.state_)
	    {
		case Inserting:
		    ledOff(INSERT);
		    break;
		case Replacing:
		    ledOff(REPLACE);
		    break;
		case Substitute:
		    ledOff(SUBSTITUTE);
		    break;
		case Multiplying:
		    ledOff(MULTIPLY);
		    break;
		default:
		    break;
	    }
	}
	loop.state_ = newState;
    }

    void shutdown() override
    {
	// Add your application's shutdown code here..


    }

    //==============================================================================
    void systemRequestedQuit() override
    {
	// This is called when the app is being asked to quit: you can ignore this
	// request and let the app carry on running, or call quit() to allow the app to close.
	quit();
    }

    void anotherInstanceStarted (const String& commandLine) override
    {
	// When another instance of the app is launched while this one is running,
	// this method is invoked, and the commandLine parameter tells you what
	// the other instance's command-line arguments were.
    }
    void suspended() override {}
    void resumed() override {}
    void unhandledException(const std::exception*, const String&, int) override { jassertfalse; }

private:
    ApplicationCommand* findApplicationCommand(const String& param)
    {
	for (auto&& cmd : commands_)
	{
	    if (cmd.param_.equalsIgnoreCase(param) || cmd.altParam_.equalsIgnoreCase(param))
	    {
		return &cmd;
	    }
	}
	return nullptr;
    }

    StringArray parseLineAsParameters(const String& line)
    {
	StringArray parameters;
	if (!line.startsWith("#"))
	{
	    StringArray tokens;
	    tokens.addTokens(line, true);
	    tokens.removeEmptyStrings(true);
	    for (String token : tokens)
	    {
		parameters.add(token.trimCharactersAtStart("\"").trimCharactersAtEnd("\""));
	    }
	}
	return parameters;
    }

    void handleVarArgCommand()
    {
	if (currentCommand_.expectedOptions_ < 0)
	{
	    executeCommand(currentCommand_);
	}
    }

    void parseParameters(StringArray& parameters)
    {
	for (String param : parameters)
	{
	    if (param == "--") continue;

	    ApplicationCommand* cmd = findApplicationCommand(param);
	    if (cmd)
	    {
		handleVarArgCommand();

		currentCommand_ = *cmd;
	    }
	    else if (currentCommand_.command_ == NONE)
	    {
		File file = File::getCurrentWorkingDirectory().getChildFile(param);
		if (file.existsAsFile())
		{
		    parseFile(file);
		}
	    }
	    else if (currentCommand_.expectedOptions_ != 0)
	    {
		currentCommand_.opts_.add(param);
		currentCommand_.expectedOptions_ -= 1;
	    }

	    // handle fixed arg commands
	    if (currentCommand_.expectedOptions_ == 0)
	    {
		executeCommand(currentCommand_);
	    }
	}

	handleVarArgCommand();
    }

    void parseFile(File file)
    {
	StringArray parameters;

	StringArray lines;
	file.readLines(lines);
	for (String line : lines)
	{
	    parameters.addArray(parseLineAsParameters(line));
	}

	parseParameters(parameters);
    }

    void sendMidiMessage(MidiOutput *midiOut, const MidiMessage&& msg)
    {
	if (midiOut)
	{
	    midiOut->sendMessageNow(msg);
	}
	else
	{
	    static bool missingOutputPortWarningPrinted = false;
	    if (!missingOutputPortWarningPrinted)
	    {
		std::cerr << "No valid MIDI output port was specified for some of the messages" << std::endl;
		missingOutputPortWarningPrinted = true;
	    }
	}
    }

    bool checkChannel(const MidiMessage& msg, int channel)
    {
	return channel == 0 || msg.getChannel() == channel;
    }

    void handleIncomingMidiMessage(MidiInput*, const MidiMessage& msg) override
    {
	if (!filterCommands_.isEmpty())
	{
	    bool filtered = false;
	    for (ApplicationCommand& cmd : filterCommands_)
	    {
		switch (cmd.command_)
		{
		    case CHANNEL:
			channel_ = asDecOrHex7BitValue(cmd.opts_[0]);
			break;
		    default:
			// no-op
			break;
		}
	    }

	    if (!filtered)
	    {
		return;
	    }
	}

	if (msg.isController()) {
	    int pedalIdx = pedalIndex(msg.getControllerValue());
	    switch (msg.getControllerNumber()) {
		case 104: // 1-10 pedal down
		    lastTime_ = (Time::getCurrentTime());
		    if (pedalIdx >= 0 && pedalIdx <= 3)
		    {
			sendMidiMessage(slMidiOut_, MidiMessage::noteOn(channel_, baseNote_+mode_+pedalIdx, (uint8)127));
		    }
		    else if (pedalIdx == RECORD)
		    {
			mode_ = mode_ > 0 ? 0 : 20;
			if (mode_ == 0) {
			    ledOff(pedalIdx);
			}
			else {
			    ledOn(pedalIdx);
			}
			updateLoops();
		    }
		    else if (pedalIdx == UNDO)
		    {
			ledOn(pedalIdx);
			sendMidiMessage(slMidiOut_, MidiMessage::noteOn(channel_, baseNote_+pedalIdx, (uint8)127));
		    }
		    else if (pedalIdx == MUTE)
		    {
			muteAll_ = !muteAll_;
			if (muteAll_)
			{
			    sendMuteAllMessage(muteAll_);
			    ledOn(MUTE_ON);
			    //sendMidiMessage(slMidiOut_, MidiMessage::noteOn(channel_, baseNote_+pedalIdx, (uint8)127));
			}
			else
			{
			    sendMuteAllMessage(muteAll_);
			    ledOff(MUTE_ON);
			    //sendMidiMessage(slMidiOut_, MidiMessage::noteOff(channel_, baseNote_+pedalIdx, (uint8)127));
			}
		    }
		    else
		    {
			sendMidiMessage(slMidiOut_, MidiMessage::noteOn(channel_, baseNote_+pedalIdx, (uint8)127));
		    }
		    break;
		case 105:
		    if (pedalIdx >= 0 && pedalIdx <= 3)
		    {
			sendMidiMessage(slMidiOut_, MidiMessage::noteOff(channel_, baseNote_+mode_+pedalIdx, (uint8)0));
		    }
		    else if (pedalIdx == RECORD)
		    {
		    }
		    else if (pedalIdx == UNDO)
		    {
			ledOff(pedalIdx);
			sendMidiMessage(slMidiOut_, MidiMessage::noteOff(channel_, baseNote_+pedalIdx, (uint8)0));
			updateLoops();
		    }
		    else if (pedalIdx == MUTE)
		    {
		    }
		    else
		    {
			sendMidiMessage(slMidiOut_, MidiMessage::noteOff(channel_, baseNote_+pedalIdx, (uint8)0));
		    }
		    break;
		default:
		    if (slMidiOut_) {
			slMidiOut_->sendMessageNow(msg);
		    }
		break;
	    }
	}
#if 0
	if (msg.isNoteOn())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "note-on         " << outputNote(msg) << " " << output7Bit(msg.getVelocity()).paddedLeft(' ', 3) << std::endl;
	}
	else if (msg.isNoteOff())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "note-off        " << outputNote(msg) << " " << output7Bit(msg.getVelocity()).paddedLeft(' ', 3) << std::endl;
	}
	else if (msg.isAftertouch())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "poly-pressure   " << outputNote(msg) << " " << output7Bit(msg.getAfterTouchValue()).paddedLeft(' ', 3) << std::endl;
	}
	else if (msg.isController())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "control-change   " << output7Bit(msg.getControllerNumber()).paddedLeft(' ', 3) << " "
	    << output7Bit(msg.getControllerValue()).paddedLeft(' ', 3) << std::endl;
	}
	else if (msg.isProgramChange())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "program-change   " << output7Bit(msg.getProgramChangeNumber()).paddedLeft(' ', 7) << std::endl;
	}
	else if (msg.isChannelPressure())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "channel-pressure " << output7Bit(msg.getChannelPressureValue()).paddedLeft(' ', 7) << std::endl;
	}
	else if (msg.isPitchWheel())
	{
	    std::cout << "channel "  << outputChannel(msg) << "   " <<
	    "pitch-bend       " << output14Bit(msg.getPitchWheelValue()).paddedLeft(' ', 7) << std::endl;
	}
	else if (msg.isMidiClock())
	{
	    std::cout << "midi-clock" << std::endl;
	}
	else if (msg.isMidiStart())
	{
	    std::cout << "start" << std::endl;
	}
	else if (msg.isMidiStop())
	{
	    std::cout << "stop" << std::endl;
	}
	else if (msg.isMidiContinue())
	{
	    std::cout << "continue" << std::endl;
	}
	else if (msg.isActiveSense())
	{
	    std::cout << "active-sensing" << std::endl;
	}
	else if (msg.getRawDataSize() == 1 && msg.getRawData()[0] == 0xff)
	{
	    std::cout << "reset" << std::endl;
	}
	else if (msg.isSysEx())
	{
	    std::cout << "system-exclusive";

	    if (!useHexadecimalsByDefault_)
	    {
		std::cout << " hex";
	    }

	    int size = msg.getSysExDataSize();
	    const uint8* data = msg.getSysExData();
	    while (size--)
	    {
		uint8 b = *data++;
		std::cout << " " << output7BitAsHex(b);
	    }

	    if (!useHexadecimalsByDefault_)
	    {
		std::cout << " dec" << std::endl;
	    }
	}
	else if (msg.isQuarterFrame())
	{
	    std::cout << "time-code " << output7Bit(msg.getQuarterFrameSequenceNumber()).paddedLeft(' ', 2) << " " << output7Bit(msg.getQuarterFrameValue()) << std::endl;
	}
	else if (msg.isSongPositionPointer())
	{
	    std::cout << "song-position " << output14Bit(msg.getSongPositionPointerMidiBeat()).paddedLeft(' ', 5) << std::endl;
	}
	else if (msg.getRawDataSize() == 2 && msg.getRawData()[0] == 0xf3)
	{
	    std::cout << "song-select " << output7Bit(msg.getRawData()[1]).paddedLeft(' ', 3) << std::endl;
	}
	else if (msg.getRawDataSize() == 1 && msg.getRawData()[0] == 0xf6)
	{
	    std::cout << "tune-request" << std::endl;
	}
#endif
    }

    String output7BitAsHex(int v)
    {
	return String::toHexString(v).paddedLeft('0', 2).toUpperCase();
    }

    String output7Bit(int v)
    {
	if (useHexadecimalsByDefault_)
	{
	    return output7BitAsHex(v);
	}
	else
	{
	    return String(v);
	}
    }

    String output14BitAsHex(int v)
    {
	return String::toHexString(v).paddedLeft('0', 4).toUpperCase();
    }

    String output14Bit(int v)
    {
	if (useHexadecimalsByDefault_)
	{
	    return output14BitAsHex(v);
	}
	else
	{
	    return String(v);
	}
    }

    String outputNote(const MidiMessage& msg)
    {
	if (noteNumbersOutput_)
	{
	    return output7Bit(msg.getNoteNumber()).paddedLeft(' ', 4);
	}
	else
	{
	    return MidiMessage::getMidiNoteName(msg.getNoteNumber(), true, true, octaveMiddleC_).paddedLeft(' ', 4);
	}
    }

    String outputChannel(const MidiMessage& msg)
    {
	return output7Bit(msg.getChannel()).paddedLeft(' ', 2);
    }

    bool tryToConnectMidiInput()
    {
	MidiInput* midi_input = nullptr;
	String midi_input_name;

	int index = MidiInput::getDevices().indexOf(midiInName_);
	if (index >= 0)
	{
	    midi_input = MidiInput::openDevice(index, this);
	    midi_input_name = midiInName_;
	}
	else
	{
	    StringArray devices = MidiInput::getDevices();
	    for (int i = 0; i < devices.size(); ++i)
	    {
		if (devices[i].containsIgnoreCase(midiInName_))
		{
		    midi_input = MidiInput::openDevice(i, this);
		    midi_input_name = devices[i];
		    break;
		}
	    }
	}

	if (midi_input)
	{
	    midi_input->start();
	    midiIn_ = midi_input;
	    fullMidiInName_ = midi_input_name;
	    return true;
	}

	return false;
    }

    bool tryToConnectOsc() {
	if (currentSendPort_ < 0) {
	    if (oscSender.connect ("127.0.0.1", oscSendPort_)) {
		std::cout << "Successfully connected to OSC Send port " << (int)oscSendPort_ << std::endl;
		currentSendPort_ = oscSendPort_;
	    }
	}

	if (currentReceivePort_ < 0) {
	    connect();
	}

	if (currentSendPort_ > 0 && currentReceivePort_ > 0) {
	    if (!pinged_)
	    {
		oscSender.send("/ping", (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/", (String) "/pingack");
	    }
	    return true;
	}

	return false;
    }

    void executeCommand(ApplicationCommand& cmd)
    {
	switch (cmd.command_)
	{
	    case NONE:
		break;
	    case LIST:
		std::cout << "MIDI Input devices:" << std::endl;
		for (auto&& device : MidiInput::getDevices())
		{
		    std::cout << device << std::endl;
		}
		std::cout << "MIDI Output devices:" << std::endl;
		for (auto&& device : MidiOutput::getDevices())
		{
		    std::cout << device << std::endl;
		}
		systemRequestedQuit();
		break;
	    case CHANNEL:
		channel_ = asDecOrHex7BitValue(cmd.opts_[0]);
		break;
	    case FCB1010_IN:
	    {
		midiIn_ = nullptr;
		midiInName_ = cmd.opts_[0];

		if (!tryToConnectMidiInput())
		{
		    std::cerr << "Couldn't find MIDI input port \"" << midiInName_ << "\", waiting." << std::endl;
		}
		break;
	    }
	    case FCB1010_OUT:
	    {
		midiOut_ = nullptr;
		midiOutName_ = cmd.opts_[0];
		int index = MidiOutput::getDevices().indexOf(midiOutName_);
		if (index >= 0)
		{
		    midiOut_ = MidiOutput::openDevice(index);
		}
		else
		{
		    StringArray devices = MidiOutput::getDevices();
		    for (int i = 0; i < devices.size(); ++i)
		    {
			if (devices[i].containsIgnoreCase(midiOutName_))
			{
			    midiOut_ = MidiOutput::openDevice(i);
			    midiOutName_ = devices[i];
			    break;
			}
		    }
		}
		if (midiOut_ == nullptr)
		{
		    std::cerr << "Couldn't find MIDI output port \"" << midiOutName_ << "\"" << std::endl;
		}
		else
		{
		    // initialize the pedal leds to off
		    for (auto i=0; i<NUM_LEDS; i++)
			ledOff(i);
		}
		break;
	    }
	    case SL_OUT:
	    {
		if (virtMidiOutName_.isNotEmpty())
		{
		    std::cerr << "Cannot have both slout and vout set in command line arguments." << std::endl;
		    break;
		}
		slMidiOut_ = nullptr;
		slMidiOutName_ = cmd.opts_[0];
		int index = MidiOutput::getDevices().indexOf(slMidiOutName_);
		if (index >= 0)
		{
		    slMidiOut_ = MidiOutput::openDevice(index);
		}
		else
		{
		    StringArray devices = MidiOutput::getDevices();
		    for (int i = 0; i < devices.size(); ++i)
		    {
			if (devices[i].containsIgnoreCase(slMidiOutName_))
			{
			    slMidiOut_ = MidiOutput::openDevice(i);
			    slMidiOutName_ = devices[i];
			    break;
			}
		    }
		}
		if (slMidiOut_ == nullptr)
		{
		    std::cerr << "Couldn't find SooperLooper MIDI input port\"" << slMidiOutName_ << "\" to output to." << std::endl;
		}
		break;
	    }
	    case VIRTUAL_OUT:
	    {
#if (JUCE_LINUX || JUCE_MAC)
		if (slMidiOutName_.isNotEmpty())
		{
		    std::cerr << "Cannot have both slout and vout set in command line arguments." << std::endl;
		    break;
		}
		slMidiOut_ = nullptr;
		virtMidiOutName_ = cmd.opts_[0];

		slMidiOut_ = MidiOutput::createNewDevice(virtMidiOutName_);
		if (slMidiOut_ == nullptr)
		{
		    std::cerr << "Couldn't create virtual MIDI output port \"" << virtMidiOutName_ << "\"" << std::endl;
		}
#else
		std::cerr << "Virtual MIDI output ports are not supported on Windows" << std::endl;
#endif
		break;
	    }
	    case BASE_NOTE:
		baseNote_ = asNoteNumber(cmd.opts_[0]);
		break;
	    case OSC_OUT:
		oscSendPort_ = asPortNumber(cmd.opts_[0]);
		// specify here where to send OSC messages to: host URL and UDP port number
		if (! oscSender.connect ("127.0.0.1", oscSendPort_))
		    std::cerr << "Error: could not connect to UDP port " << cmd.opts_[0] << std::endl;
		else
		    currentSendPort_ = oscSendPort_;
		break;
	    case OSC_IN:
		oscReceivePort_ = asPortNumber(cmd.opts_[0]);
		if (!tryToConnectOsc())
		    std::cerr << "Error: could not connect to UDP port " << cmd.opts_[0] << std::endl;
		break;
	    default:
		filterCommands_.add(cmd);
		break;
	}
    }

    uint16 asPortNumber(String value)
    {
	return (uint16)limit16Bit(asDecOrHexIntValue(value));
    }

    uint8 asNoteNumber(String value)
    {
	if (value.length() >= 2)
	{
	    value = value.toUpperCase();
	    String first = value.substring(0, 1);
	    if (first.containsOnly("CDEFGABH") && value.substring(value.length()-1).containsOnly("1234567890"))
	    {
		int note = 0;
		switch (first[0])
		{
		    case 'C': note = 0; break;
		    case 'D': note = 2; break;
		    case 'E': note = 4; break;
		    case 'F': note = 5; break;
		    case 'G': note = 7; break;
		    case 'A': note = 9; break;
		    case 'B': note = 11; break;
		    case 'H': note = 11; break;
		}

		if (value[1] == 'B')
		{
		    note -= 1;
		}
		else if (value[1] == '#')
		{
		    note += 1;
		}

		note += (value.getTrailingIntValue() + 5 - octaveMiddleC_) * 12;

		return (uint8)limit7Bit(note);
	    }
	}

	return (uint8)limit7Bit(asDecOrHexIntValue(value));
    }

    uint8 asDecOrHex7BitValue(String value)
    {
	return (uint8)limit7Bit(asDecOrHexIntValue(value));
    }

    uint16 asDecOrHex14BitValue(String value)
    {
	return (uint16)limit14Bit(asDecOrHexIntValue(value));
    }

    int asDecOrHexIntValue(String value)
    {
	if (value.endsWithIgnoreCase("H"))
	{
	    return value.dropLastCharacters(1).getHexValue32();
	}
	else if (value.endsWithIgnoreCase("M"))
	{
	    return value.getIntValue();
	}
	else if (useHexadecimalsByDefault_)
	{
	    return value.getHexValue32();
	}
	else
	{
	    return value.getIntValue();
	}
    }

    static uint8 limit7Bit(int value)
    {
	return (uint8)jlimit(0, 0x7f, value);
    }

    static uint16 limit14Bit(int value)
    {
	return (uint16)jlimit(0, 0x3fff, value);
    }

    static uint16 limit16Bit(int value)
    {
	return (uint16)jlimit(0, 0xffff, value);
    }

    int pedalIndex(int controllerValue) {
	switch (controllerValue) {
	    case 1:
	    case 2:
	    case 3:
	    case 4:
	    case 5:
	    case 6:
	    case 7:
	    case 8:
	    case 9:
		return controllerValue-1;
	    case 0:
		return 9;
	    case 10:
		return UP;
	    case 11:
		return DOWN;
	    default:
		return controllerValue;
	}
    }

    uint8 ledNumber(int pedalIdx) {
	switch (pedalIdx) {
	    case 0:
	    case 1:
	    case 2:
	    case 3:
	    case 4:
	    case 5:
	    case 6:
	    case 7:
	    case 8:
		return pedalIdx+1;
	    case 9:
		return 0;
	    default:
		return pedalIdx+1;
	}
    }

    void ledOn(int pedalIdx) {
	sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 106, ledNumber(pedalIdx)));
	leds_.getReference(pedalIdx).on_ = true;
    }

    void ledOff(int pedalIdx) {
	sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 107, ledNumber(pedalIdx)));
	leds_.getReference(pedalIdx).on_ = false;
    }

    void selectLoop() {
	sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 108, selectedLoop_ + 1));
    }

    void getCurrentState(int index)
    {
	String buf = "/sl";
	buf = buf + "/" + std::to_string(index) + "/get";
	oscSender.send(buf,
		       (String) "state",
		       (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/",
		       (String) "/ctrl");
    }

    void sendMuteAllMessage(bool on)
    {
	String buf = "/sl/-1/hit";
	if (on)
	{
	    oscSender.send(buf, (String) "mute_on");
	}
	else
	{
	    oscSender.send(buf, (String) "mute_off");
	}
    }

    void registerAutoUpdates(int index, bool unreg)
    {
	String buf = "/sl";
	if (unreg)
	{
	    buf = buf + "/" + std::to_string(index) + "/unregister_auto_update";
	    oscSender.send(buf,
			   (String) "state",
			   (int)100,
			   (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/",
			   (String) "/ctrl");
	}
	else{
	    buf = buf + "/" + std::to_string(index) + "/register_auto_update";
	    oscSender.send(buf,
			   (String) "state",
			   (int)100,
			   (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/",
			   (String) "/ctrl");
	}
    }

    void registerGlobalUpdates(bool unreg)
    {
	String buf = "";
	if (unreg)
	{
	    buf = buf + "/unregister_update";
	    oscSender.send(buf,
			   (String) "selected_loop_num",
			   (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/",
			   (String) "/ctrl");
	}
	else{
	    buf = buf + "/register_update";
	    oscSender.send(buf,
			   (String) "selected_loop_num",
			   (String) "osc.udp://localhost:" + std::to_string(currentReceivePort_) + "/",
			   (String) "/ctrl");
	}

    }

    void handlePingAckMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    int i = 0;
	    for (OSCArgument* arg = message.begin(); arg != message.end(); ++arg)
	    {
		switch (i)
		{
		    case 0:
			if (arg->isString())
			    hostUrl_ = arg->getString();
			break;
		    case 1:
			if (arg->isString())
			    version_ = arg->getString();
			break;
		    case 2:
			if (arg->isInt32())
			    loopCount_ = arg->getInt32();
			break;
		    case 3:
			if (arg->isInt32())
			    engineId_ = arg->getInt32();
			break;
		    default:
			std::cerr << "Unexpected number of arguments for /pingack" << std::endl;
		}
		i++;
	    }

	    if (loopCount_ > 0)
	    {
		loops_.clear();
		for (i = 0; i < loopCount_; i++)
		{
		    loops_.add({i, Off, leds_.getReference(i)});
		    registerAutoUpdates(i, false);
		    getCurrentState(i);
		}
		registerGlobalUpdates(false);
	    }
	    heartbeat_ = 5; // we just heard from the looper
	}
    }

    void handleHeartbeatMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    int i = 0;
	    int numloops = 0;
	    int uid = engineId_;
	    for (OSCArgument* arg = message.begin(); arg != message.end(); ++arg)
	    {
		switch (i)
		{
		    case 0:
			if (arg->isString())
			    hostUrl_ = arg->getString();
			break;
		    case 1:
			if (arg->isString())
			    version_ = arg->getString();
			break;
		    case 2:
			if (arg->isInt32())
			    numloops = arg->getInt32();
			break;
		    case 3:
			if (arg->isInt32())
			    uid = arg->getInt32();
			break;
		    default:
			std::cerr << "Unexpected number of arguments for /heartbeat" << std::endl;
		}
		i++;
	    }

	    if (uid != engineId_) {
		// looper changed on us, reinitialize
		if (numloops > 0)
		{
		    loopCount_ = numloops;
		    loops_.clear();
		    for (i = 0; i < loopCount_; i++)
		    {
			loops_.add({i, Off, leds_.getReference(i)});
			registerAutoUpdates(i, false);
			getCurrentState(i);
			updateLoops();
		    }
		    registerGlobalUpdates(false);
		}
	    }
	    else
	    {
		// check loopcount
		if (loopCount_ != numloops)
		{
		    for (auto i=loopCount_; i<numloops; i++)
		    {
			registerAutoUpdates(i, false);
			loops_.add({i, Off, leds_.getReference(i)});
			updateLoops();
		    }
		    loopCount_ = numloops;
		}
	    }

	    if (heartbeatOn_)
		ledOn(HEARTBEAT);
	    else
		ledOff(HEARTBEAT);
	    heartbeatOn_ = !heartbeatOn_;
	    heartbeat_ = 5; // we just heard from the looper
	}
    }
    void handleCtrlMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    OSCArgument* arg = message.begin();
	    int loopIndex = -1;
	    int loopState = 0;
	    if (arg->isInt32())
	    {
		loopIndex = arg->getInt32();
		++arg;
	    }
	    else {
		std::cerr << "unrecognized format for ctrl message." << std::endl;
		return;
	    }

	    if (loopIndex == -2)
	    {
		// global control update
		if (arg->isString() && arg->getString() == "selected_loop_num")
		{
		    ++arg;
		    if (arg->isFloat32())
		    {
			selectedLoop_ = arg->getFloat32();
			selectLoop();
		    }
		}
	    }
	    else if (loopIndex < 0) {
		return;
	    }
	    else
	    {
		if (arg->isString() && arg->getString() == "state")
		{
		    ++arg;
		    if (arg->isFloat32())
		    {
			loopState = arg->getFloat32();
			Loop &loop = loops_.getReference(loopIndex);
			updateLoopLedState(loop, static_cast<LoopStates>(loopState));
		    }
		}
		heartbeat_ = 5; // we just heard from the looper
	    }
	}
    }

    void oscMessageReceived (const OSCMessage& message) override
    {
	if (!message.getAddressPattern().toString().startsWith("/heartbeat"))
	{
	    std::cout << "-" <<
	    + "- osc message, address = '"
	    + message.getAddressPattern().toString()
	    + "', "
	    + String (message.size())
	    + " argument(s)" << std::endl;

	    for (OSCArgument* arg = message.begin(); arg != message.end(); ++arg)
	    {
		String typeAsString;
		String valueAsString;

		if (arg->isFloat32())
		{
		    typeAsString = "float32";
		    valueAsString = String (arg->getFloat32());
		}
		else if (arg->isInt32())
		{
		    typeAsString = "int32";
		    valueAsString = String (arg->getInt32());
		}
		else if (arg->isString())
		{
		    typeAsString = "string";
		    valueAsString = arg->getString();
		}
		else if (arg->isBlob())
		{
		    typeAsString = "blob";
		    auto& blob = arg->getBlob();
		    valueAsString = String::fromUTF8 ((const char*) blob.getData(), (int) blob.getSize());
		}
		else
		{
		    typeAsString = "(unknown)";
		}

		std::cout << "==- " + typeAsString.paddedRight(' ', 12) + valueAsString << std::endl;

	    }
	}
	if (message.getAddressPattern().toString().startsWith("/pingack"))
	{
	    handlePingAckMessage(message);
	}
	else if (message.getAddressPattern().toString().startsWith("/ctrl"))
	{
	    handleCtrlMessage(message);
	}
	else if (message.getAddressPattern().toString().startsWith("/heartbeat"))
	{
	    handleHeartbeatMessage(message);
	}
    }

    void oscBundleReceived (const OSCBundle& bundle) override
    {

    }

    void connect()
    {
	auto portToConnect = oscReceivePort_;

	if (! isValidOscPort (portToConnect))
	{
	    handleInvalidPortNumberEntered();
	    return;
	}

	if (oscReceiver.connect (portToConnect))
	{
	    currentReceivePort_ = portToConnect;
	    oscReceiver.addListener (this);
	    oscReceiver.registerFormatErrorHandler ([this] (const char* data, int dataSize)
						    {
							std::cerr << "- (" + String(dataSize) + "bytes with invalid format)" << std::endl;
						    });
	    //connectButton.setButtonText ("Disconnect");
	}
	else
	{
	    handleConnectError (portToConnect);
	}
    }

    void disconnect()
    {
	if (oscReceiver.disconnect())
	{
	    currentReceivePort_ = -1;
	    oscReceiver.removeListener (this);
	    //connectButton.setButtonText ("Connect");
	}
	else
	{
	    handleDisconnectError();
	}
    }

    void handleConnectError (int failedPort)
    {
	std::cerr << "Error: could not connect to port " + String (failedPort) << std::endl;
    }

    void handleDisconnectError()
    {
	std::cerr << "An unknown error occured while trying to disconnect from UDP port." << std::endl;
    }

    void handleInvalidPortNumberEntered()
    {
	std::cout << "Error: you have entered an invalid UDP port number." << std::endl;
    }

    bool isConnected() const
    {
	return currentReceivePort_ != -1;
    }

    bool isValidOscPort (int port) const
    {
	return port > 0 && port < 65536;
    }

    void printVersion()
    {
	std::cout << ProjectInfo::projectName << " v" << ProjectInfo::versionString << std::endl;
	std::cout << "https://github.com/atinm/loop4r_control" << std::endl;
    }

    void printUsage()
    {
	printVersion();
	std::cout << std::endl;
	std::cout << "Usage: " << ProjectInfo::projectName << " [ commands ] [ programfile ] [ -- ]" << std::endl << std::endl
	<< "Commands:" << std::endl;
	for (auto&& cmd : commands_)
	{
	    std::cout << "  " << cmd.param_.paddedRight(' ', 5);
	    if (cmd.optionsDescription_.isNotEmpty())
	    {
		std::cout << " " << cmd.optionsDescription_.paddedRight(' ', 13);
	    }
	    else
	    {
		std::cout << "              ";
	    }
	    std::cout << "  " << cmd.commandDescription_;
	    std::cout << std::endl;
	}
	std::cout << "  -h  or  --help       Print Help (this message) and exit" << std::endl;
	std::cout << "  --version            Print version information and exit" << std::endl;
	std::cout << "  --                   Read commands from standard input until it's closed" << std::endl;
	std::cout << std::endl;
	std::cout << "Alternatively, you can use the following long versions of the commands:" << std::endl;
	String line = " ";
	for (auto&& cmd : commands_)
	{
	    if (cmd.altParam_.isNotEmpty())
	    {
		if (line.length() + cmd.altParam_.length() + 1 >= 80)
		{
		    std::cout << line << std::endl;
		    line = " ";
		}
		line << " " << cmd.altParam_;
	    }
	}
	std::cout << line << std::endl << std::endl;
	std::cout << "By default, numbers are interpreted in the decimal system, this can be changed" << std::endl
	<< "to hexadecimal by sending the \"hex\" command. Additionally, by suffixing a " << std::endl
	<< "number with \"M\" or \"H\", it will be interpreted as a decimal or hexadecimal" << std::endl
	<< "respectively." << std::endl;
	std::cout << std::endl;
	std::cout << "The MIDI device name doesn't have to be an exact match." << std::endl;
	std::cout << "If " << getApplicationName() << " can't find the exact name that was specified, it will pick the" << std::endl
	<< "first MIDI output port that contains the provided text, irrespective of case." << std::endl;
	std::cout << std::endl;
    }

    OSCReceiver oscReceiver;
    OSCSender oscSender;

    int currentReceivePort_ = -1;
    int currentSendPort_ = -1;
    int channel_;
    int baseNote_;
    int selected_;
    int oscSendPort_;
    int oscReceivePort_;
    int engineId_;
    int mode_;

    Array<Loop> loops_;
    Array<LED> leds_;
    Array<ApplicationCommand> commands_;
    Array<ApplicationCommand> filterCommands_;

    bool noteNumbersOutput_;
    int octaveMiddleC_;
    bool useHexadecimalsByDefault_;

    String midiInName_;
    ScopedPointer<MidiInput> midiIn_;
    String fullMidiInName_;

    String midiOutName_;
    ScopedPointer<MidiOutput> midiOut_;

    String slMidiOutName_;
    ScopedPointer<MidiOutput> slMidiOut_;
    String virtMidiOutName_; // either slMidiOutName_ or virtMidiOutName_ are used, never both

    int loopCount_;
    int selectedLoop_;
    bool pinged_;
    String hostUrl_;
    String version_;
    int heartbeat_;
    bool muteAll_ = false;
    bool heartbeatOn_ = false;

    ApplicationCommand currentCommand_;
    Time lastTime_;
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION (loop4r_controlApplication)
