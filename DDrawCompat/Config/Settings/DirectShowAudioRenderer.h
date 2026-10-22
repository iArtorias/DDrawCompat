#pragma once

#include <Config/EnumSetting.h>

namespace Config
{
	namespace Settings
	{
		class DirectShowAudioRenderer : public EnumSetting
		{
		public:
			enum Values
			{
				APP, WAVEOUT
			};

			DirectShowAudioRenderer()
				: EnumSetting( "DirectShowAudioRenderer", "app", { "app", "waveout" } )
			{}
		};
	}

	extern Settings::DirectShowAudioRenderer directShowAudioRenderer;
}