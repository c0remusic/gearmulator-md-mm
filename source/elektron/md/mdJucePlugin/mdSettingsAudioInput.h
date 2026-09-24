#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"
#include "juce_events/juce_events.h"

#include <string>

namespace Rml { class Element; }
namespace jucePluginEditorLib { class Processor; }

namespace mdJucePlugin
{
	// Machinedrum DSP/Audio settings: audio input status and the parallel
	// transport switch.
	class SettingsAudioInput final : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsAudioInput(jucePluginEditorLib::Processor& _processor, Rml::Element* _root);
		~SettingsAudioInput() override;

	private:
		void timerCallback() override;
		void updateTransportStatus();

		jucePluginEditorLib::Processor& m_processor;
		Rml::Element* m_status;
		Rml::Element* m_settings;
		Rml::Element* m_transportStatus;
		std::string m_lastStatus;
		std::string m_lastTransportStatus;
	};
}
