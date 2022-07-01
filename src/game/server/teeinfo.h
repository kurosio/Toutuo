#ifndef GAME_SERVER_TEEINFO_H
#define GAME_SERVER_TEEINFO_H

class CTeeInfo
{
public:
	constexpr static const float DARKEST_LGT_7 = 61 / 255.0f;

	char m_SkinName[64] = {'\0'};
	int m_UseCustomColor = 0;
	int m_ColorBody = 0;
	int m_ColorFeet = 0;

	CTeeInfo() = default;

	CTeeInfo(const char *pSkinName, int UseCustomColor, int ColorBody, int ColorFeet);
};
#endif //GAME_SERVER_TEEINFO_H
