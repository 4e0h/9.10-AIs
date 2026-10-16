#include "pch.h"
#include "../Public/Client.h"
#include "../../../Four_E_Zero_H/Four_E_Zero_H/Public/Configuration.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlaylistAthena.h"
#include "../../../Four_E_Zero_H/Four_E_Zero_H/Public/NoKillerMessages.h"
#include <string>
#include <atomic>
#include <vector>
#include <cwctype>

bool bEOREnabled = true;
bool bDisablePreEdits = false;
inline void* (*SelectResetOG)(void*) = nullptr;
inline void* (*SelectEditOG)(void*) = nullptr;
inline void (*PerformBuildingEditInteractionOG)(void*) = nullptr;
inline void (*CompleteBuildingEditInteraction)(void*) = nullptr;

void* SelectEdit(void* a1)
{
    void* result = SelectEditOG(a1);

    if (bEOREnabled && CompleteBuildingEditInteraction)
        CompleteBuildingEditInteraction(a1);

    return result;
}

void* SelectReset(void* a1)
{
    void* result = SelectResetOG(a1);

    if (bEOREnabled && CompleteBuildingEditInteraction)
        CompleteBuildingEditInteraction(a1);

    return result;
}
void PerformBuildingEditInteraction(AFortPlayerControllerAthena* _this)
{
    if (bDisablePreEdits && _this->TargetedBuilding->IsA<ABuildingPlayerPrimitivePreview>())
        return;

    return PerformBuildingEditInteractionOG(_this);
}

void JoinLocalServer()
{
    auto World = UWorld::GetWorld();

    if (!World)
        return;

    std::wstring Command = L"open 127.0.0.1:" + std::to_wstring((int)FConfiguration::Port);

    UKismetSystemLibrary::ExecuteConsoleCommand(World, FString(Command.c_str()), nullptr);
}

static std::atomic<DWORD> GGameThreadId{ 0 };

static void (*ProcessEventOG)(UObject*, UFunction*, void*) = nullptr;
static int GInOurCode = 0;

static bool GKillFeedBroken = false;
static bool GReportsBroken = false;
static bool GStormReportBroken = false;
static int GFaults = 0;
static bool GFaultToReport = false;

static FName GNameOnRepDeathInfo;
static FName GNameOnRepThankedBusDriver;
static FName GNameUpdateKillFeed;
static FName GNameClientReportKill;
static FName GNameBatchedDamageCues;

struct FGameWindow
{
    HWND Window = nullptr;
    DWORD Thread = 0;
};

static BOOL CALLBACK FindGameWindow(HWND Window, LPARAM Out)
{
    DWORD Pid = 0;
    const DWORD Thread = GetWindowThreadProcessId(Window, &Pid);

    if (Pid != GetCurrentProcessId())
        return TRUE;

    char ClassName[64] = {};
    GetClassNameA(Window, ClassName, sizeof(ClassName));

    if (strcmp(ClassName, "UnrealWindow") != 0)
        return TRUE;

    ((FGameWindow*)Out)->Window = Window;
    ((FGameWindow*)Out)->Thread = Thread;
    return FALSE;
}

static AFortPlayerControllerAthena* LocalController()
{
    auto World = UWorld::GetWorld();

    if (!World || !World->OwningGameInstance)
        return nullptr;

    auto& LocalPlayers = World->OwningGameInstance->LocalPlayers;

    if (LocalPlayers.Num() <= 0 || !LocalPlayers[0])
        return nullptr;

    return (AFortPlayerControllerAthena*)LocalPlayers[0]->PlayerController;
}

static std::wstring OneDecimal(double Value)
{
    wchar_t Out[48];
    swprintf(Out, 48, L"%.1f", Value);
    return std::wstring(Out);
}

static void TellServer(const std::wstring& Text)
{
    static UFunction* Fn = nullptr;
    static uint32 Offset = 0;
    static uint32 Size = 0;
    static bool bLooked = false;
    static double Tokens = 12.0;
    static ULONGLONG LastRefill = 0;

    if (GReportsBroken)
        return;

    auto World = UWorld::GetWorld();
    if (!World || !World->NetDriver)
        return;

    auto Controller = LocalController();
    if (!Controller)
        return;

    if (!bLooked)
    {
        bLooked = true;
        Fn = Controller->GetFunction("ServerCheat");

        if (Fn)
        {
            auto Params = Fn->GetParamsNamed();
            Size = Params.Size;

            if (!Params.NameOffsetMap.empty())
                Offset = Params.NameOffsetMap[0].Offset;
        }
    }

    if (!Fn || Size == 0 || Size > 0x100 || Offset + 0x10 > Size)
        return;

    const ULONGLONG Now = GetTickCount64();

    if (LastRefill == 0)
        LastRefill = Now;

    Tokens += (double)(Now - LastRefill) / 1000.0 * 6.0;
    LastRefill = Now;

    if (Tokens > 12.0)
        Tokens = 12.0;

    if (Tokens < 1.0)
        return;

    Tokens -= 1.0;

    std::wstring Line = L"clientlog " + Text;

    if (Line.size() > 400)
        Line.resize(400);

    FString Param(Line.c_str());

    alignas(16) uint8 Buffer[0x110] = {};
    memcpy(Buffer + Offset, (const void*)&Param, 0x10);

    Controller->ProcessEvent(Fn, Buffer);

    if (Param.Data)
        FMemory::Free(Param.Data);
}

static int32 ReflectedOffset(const UObject* Object, const char* Name)
{
    if (!Object)
        return -1;

    const uint32 Offset = Object->GetOffset(Name);
    return Offset == (uint32)-1 ? -1 : (int32)Offset;
}

static std::wstring PlayerNameOf(const UObject* PlayerState)
{
    static int32 Offset = -2;
    static UFunction* GetName = nullptr;
    static bool bLookedForGetName = false;
    static const UClass* PlayerStateClass = nullptr;

    if (!PlayerState)
        return std::wstring();

    if (!PlayerStateClass)
        PlayerStateClass = FindClass("PlayerState");

    if (PlayerStateClass && !PlayerState->IsA(PlayerStateClass))
        return std::wstring();

    if (!bLookedForGetName)
    {
        bLookedForGetName = true;
        GetName = PlayerState->GetFunction("GetPlayerName");

        if (GetName && GetName->GetPropertiesSize() != 0x10)
            GetName = nullptr;
    }

    if (GetName)
    {
        alignas(16) uint8 Params[0x10] = {};
        ((UObject*)PlayerState)->ProcessEvent(GetName, Params);

        auto& Returned = *(FString*)Params;
        std::wstring Name = (Returned.CStr() && Returned.Num() > 1) ? std::wstring(Returned.CStr()) : std::wstring();

        if (Returned.Data)
            FMemory::Free(Returned.Data);

        if (!Name.empty())
            return Name;
    }

    if (Offset == -2)
    {
        Offset = ReflectedOffset(PlayerState, "PlayerNamePrivate");

        if (Offset < 0)
            Offset = ReflectedOffset(PlayerState, "PlayerName");
    }

    if (Offset < 0)
        return std::wstring();

    auto& Name = GetFromOffset<FString>(PlayerState, (uint32)Offset);

    if (!Name.CStr() || Name.Num() <= 1)
        return std::wstring();

    return std::wstring(Name.CStr());
}

struct FNoKillerDeath
{
    std::wstring Name;
    ULONGLONG At = 0;
};

static std::vector<FNoKillerDeath> GNoKillerDeaths;
static std::vector<const void*> GKillFeedSeen;
static UObject* GKillFeedWidget = nullptr;
static int32 GKillFeedWidgetIndex = -1;

static int32 GDeathInfoOffset = -2, GFinisherOffset = -2, GDeathCauseOffset = -2, GKillFeedOffset = -2;

static void OnDeathInfoArrived(UObject* PlayerState)
{
    if (GDeathInfoOffset == -2)
    {
        GDeathInfoOffset = ReflectedOffset(PlayerState, "DeathInfo");

        auto Struct = FindStruct("DeathInfo");
        const uint32 Finisher = Struct ? Struct->GetOffset("FinisherOrDowner") : (uint32)-1;
        const uint32 Cause = Struct ? Struct->GetOffset("DeathCause") : (uint32)-1;

        GFinisherOffset = Finisher == (uint32)-1 ? -1 : (int32)Finisher;
        GDeathCauseOffset = Cause == (uint32)-1 ? -1 : (int32)Cause;
    }

    if (GDeathInfoOffset < 0 || GFinisherOffset < 0)
        return;

    auto Finisher = GetFromOffset<UObject*>(PlayerState, (uint32)(GDeathInfoOffset + GFinisherOffset));

    if (Finisher && Finisher != PlayerState)
        return;

    const std::wstring Name = PlayerNameOf(PlayerState);
    if (Name.empty())
        return;

    GNoKillerDeaths.push_back({ Name, GetTickCount64() });
}

static FText MakeText(const std::wstring& Text)
{
    return UKismetTextLibrary::Conv_StringToText(FString(Text.c_str()));
}

static std::wstring TextToWide(FText& Text)
{
    FString String = UKismetTextLibrary::Conv_TextToString(Text);

    std::wstring Wide = (String.CStr() && String.Num() > 0) ? std::wstring(String.CStr()) : std::wstring();

    if (String.Data)
        FMemory::Free(String.Data);

    return Wide;
}

static bool LineNames(const std::wstring& Line, const std::wstring& Name)
{
    auto IsWordChar = [](wchar_t Ch) { return iswalnum(Ch) || Ch == L'_'; };

    for (size_t At = Line.find(Name); At != std::wstring::npos; At = Line.find(Name, At + 1))
    {
        const bool bStartOk = At == 0 || !IsWordChar(Line[At - 1]);
        const bool bEndOk = At + Name.size() >= Line.size() || !IsWordChar(Line[At + Name.size()]);

        if (bStartOk && bEndOk)
            return true;
    }

    return false;
}

struct FKillFeedExpect
{
    std::wstring Victim;
    std::wstring Killer;
};

static std::vector<FKillFeedExpect> GKillFeedExpect;
static int GKillFeedNamesRight = 0;
static int GKillFeedNamesFixed = 0;
static int GKillFeedNamesChecked = 0;
static int GKillFeedNamesUnchecked = 0;

static int ReadBoolByName(const UObject* Object, const char* Name);

static void ExpectKillFeedLine(UObject* PlayerState, bool bDeath)
{
    FKillFeedExpect Expect;
    Expect.Victim = PlayerNameOf(PlayerState);

    if (bDeath && GDeathInfoOffset >= 0 && GFinisherOffset >= 0)
    {
        auto Finisher = GetFromOffset<UObject*>(PlayerState, (uint32)(GDeathInfoOffset + GFinisherOffset));

        if (Finisher && Finisher != PlayerState)
            Expect.Killer = PlayerNameOf(Finisher);
    }

    GKillFeedExpect.push_back(Expect);
}

static bool FindTagContent(const std::wstring& Line, const std::wstring& Tag, size_t& Start, size_t& Length)
{
    size_t Open = std::wstring::npos;
    size_t TagLength = 0;

    if (!Tag.empty())
    {
        Open = Line.find(Tag);
        TagLength = Tag.size();
    }
    else
    {
        for (size_t At = Line.find(L'<'); At != std::wstring::npos; At = Line.find(L'<', At + 1))
        {
            const size_t End = Line.find(L'>', At);
            if (End == std::wstring::npos)
                break;

            const std::wstring Found = Line.substr(At, End - At + 1);

            if (Found == L"</>" || Found == L"<Victim>" || (Found.size() >= 2 && Found[Found.size() - 2] == L'/') ||
                Found.compare(0, 4, L"<img") == 0)
                continue;

            Open = At;
            TagLength = Found.size();
            break;
        }
    }

    if (Open == std::wstring::npos)
        return false;

    Start = Open + TagLength;

    const size_t Close = Line.find(L"</>", Start);
    if (Close == std::wstring::npos)
        return false;

    Length = Close - Start;
    return true;
}

static std::vector<std::wstring> PlayerNamesOnScreen()
{
    std::vector<std::wstring> Names;

    auto World = UWorld::GetWorld();
    if (!World || !World->GameState)
        return Names;

    auto GameState = (UObject*)World->GameState;
    const int32 ArrayOffset = ReflectedOffset(GameState, "PlayerArray");
    if (ArrayOffset < 0)
        return Names;

    auto& Players = GetFromOffset<TArray<UObject*>>(GameState, (uint32)ArrayOffset);

    if (Players.Num() <= 0 || Players.Num() > 200 || !Players.Data)
        return Names;

    for (int i = 0; i < Players.Num(); i++)
    {
        if (!Players.Data[i])
            continue;

        const std::wstring Name = PlayerNameOf(Players.Data[i]);

        if (!Name.empty())
            Names.push_back(Name);
    }

    return Names;
}

static int PutNameInTag(std::wstring& Line, const std::wstring& Tag, const std::wstring& Name, std::wstring& Was)
{
    size_t Start = 0, Length = 0;

    if (Name.empty() || !FindTagContent(Line, Tag, Start, Length))
        return -1;

    Was = Line.substr(Start, Length);

    if (Was == Name)
        return 0;

    const auto Names = PlayerNamesOnScreen();

    if (std::find(Names.begin(), Names.end(), Was) == Names.end())
        return -1;

    Line.replace(Start, Length, Name);
    return 1;
}

static bool FixKillFeedNames(const std::wstring& Native, std::wstring& Out, std::wstring& Note)
{
    if (GKillFeedExpect.empty())
        return false;

    const FKillFeedExpect& Expect = GKillFeedExpect.back();

    Out = Native;

    std::wstring WasVictim, WasKiller;
    const int Victim = PutNameInTag(Out, L"<Victim>", Expect.Victim, WasVictim);
    const int Killer = Expect.Killer.empty() ? -1 : PutNameInTag(Out, L"", Expect.Killer, WasKiller);

    const bool bVictim = Victim == 1;
    const bool bKiller = Killer == 1;

    if (Victim < 0 && Killer < 0)
    {
        GKillFeedNamesUnchecked++;
        return false;
    }

    GKillFeedNamesChecked++;

    if (!bVictim && !bKiller)
    {
        GKillFeedNamesRight++;
        return false;
    }

    GKillFeedNamesFixed++;
    Note = L" (the game's own line was \"" + Native + L"\" -- it named " + (bVictim ? WasVictim : WasKiller) + L" for " +
           (bVictim ? Expect.Victim : Expect.Killer) + L"; the name put right)";
    return true;
}

static void ReportPlayerIds(const wchar_t* When)
{
    auto World = UWorld::GetWorld();
    if (!World || !World->GameState)
        return;

    auto GameState = (UObject*)World->GameState;
    const int32 ArrayOffset = ReflectedOffset(GameState, "PlayerArray");
    if (ArrayOffset < 0)
        return;

    auto& Players = GetFromOffset<TArray<UObject*>>(GameState, (uint32)ArrayOffset);

    if (Players.Num() <= 0 || Players.Num() > 200 || !Players.Data)
        return;

    int Total = 0, Bots = 0, WithId = 0, BotsWithId = 0;
    std::vector<std::wstring> Names;
    std::vector<int32> Numbers;

    for (int i = 0; i < Players.Num(); i++)
    {
        UObject* PS = Players.Data[i];
        if (!PS)
            continue;

        Total++;

        const bool bBot = ReadBoolByName(PS, "bIsABot") == 1;
        if (bBot)
            Bots++;

        const int32 IdOffset = ReflectedOffset(PS, "UniqueId");
        const void* IdObject = IdOffset >= 0 ? *(const void* const*)((const uint8*)PS + IdOffset + 8) : nullptr;

        if (IdObject)
        {
            WithId++;

            if (bBot)
                BotsWithId++;
        }

        const std::wstring Name = PlayerNameOf(PS);

        if (!Name.empty() && std::find(Names.begin(), Names.end(), Name) == Names.end())
            Names.push_back(Name);

        const int32 NumberOffset = ReflectedOffset(PS, "PlayerId");

        if (NumberOffset >= 0)
        {
            const int32 Number = GetFromOffset<int32>(PS, (uint32)NumberOffset);

            if (std::find(Numbers.begin(), Numbers.end(), Number) == Numbers.end())
                Numbers.push_back(Number);
        }
    }

    TellServer(std::wstring(L"player ids on this screen ") + When + L": " + std::to_wstring(Total) + L" player(s), " + std::to_wstring(Bots) +
               L" of them bots | with a player id: " + std::to_wstring(WithId) + L" (bots: " + std::to_wstring(BotsWithId) + L" of " +
               std::to_wstring(Bots) + L") | different names: " + std::to_wstring(Names.size()) + L", different player numbers: " +
               std::to_wstring(Numbers.size()) + L" | killfeed lines checked against the player they are about: " +
               std::to_wstring(GKillFeedNamesChecked) + L" (" + std::to_wstring(GKillFeedNamesRight) +
               L" right, " + std::to_wstring(GKillFeedNamesFixed) + L" put right; " + std::to_wstring(GKillFeedNamesUnchecked) +
               L" could not be checked)");
}

static bool LookAtTheKillFeed()
{
    auto World = UWorld::GetWorld();
    if (!World || !World->GameState)
        return false;

    auto GameState = (UObject*)World->GameState;

    static const void* KillFeedClass = nullptr;

    if (GKillFeedOffset == -2 || (const void*)GameState->Class != KillFeedClass)
    {
        KillFeedClass = (const void*)GameState->Class;
        GKillFeedOffset = ReflectedOffset(GameState, "KillFeedEntry");
        GKillFeedSeen.clear();

        if (GKillFeedOffset < 0 && GameState->Class && GameState->Class->Name.ToString().find("Athena") != UEAllocatedString::npos)
            TellServer(L"killfeed: this build's game state has no KillFeedEntry -- the \"no killer\" lines cannot go in the killfeed.");
    }

    if (GKillFeedOffset < 0)
        return false;

    auto& Lines = GetFromOffset<TArray<FText>>(GameState, (uint32)GKillFeedOffset);

    if (Lines.Num() <= 0 || Lines.Num() > 64 || !Lines.Data)
    {
        GKillFeedSeen.clear();
        return false;
    }

    const ULONGLONG Now = GetTickCount64();
    bool bSwapped = false;
    std::vector<const void*> StillThere;

    int NewLines = 0;

    for (int i = 0; i < Lines.Num(); i++)
    {
        const void* LineKey = *(const void* const*)&Lines.Data[i];

        if (LineKey && std::find(GKillFeedSeen.begin(), GKillFeedSeen.end(), LineKey) == GKillFeedSeen.end())
            NewLines++;
    }

    for (int i = 0; i < Lines.Num(); i++)
    {
        FText& Line = Lines.Data[i];
        const void* Key = *(const void* const*)&Line;

        if (!Key)
            continue;

        if (std::find(GKillFeedSeen.begin(), GKillFeedSeen.end(), Key) != GKillFeedSeen.end())
        {
            StillThere.push_back(Key);
            continue;
        }

        const std::wstring Native = TextToWide(Line);
        std::wstring Shown = Native;
        std::wstring Note;

        {
            std::wstring Fixed, FixNote;

            if (NewLines == 1 && FixKillFeedNames(Native, Fixed, FixNote))
            {
                FText NewText = MakeText(Fixed);
                memcpy((void*)&Line, (const void*)&NewText, sizeof(FText));

                Key = *(const void* const*)&Line;
                Shown = Fixed;
                Note = FixNote;
                bSwapped = true;
            }
        }

        static int LinesSeen = 0;
        if (++LinesSeen == 1)
            ReportPlayerIds(L"at the first killfeed line");

        for (size_t d = 0; d < GNoKillerDeaths.size(); d++)
        {
            const auto& Death = GNoKillerDeaths[d];

            if (!LineNames(Shown, Death.Name))
                continue;

            const std::wstring Before = Shown;
            Shown = NoKillerMessages::LineForKillFeed(NoKillerMessages::PickFor(Death.Name.c_str()), Death.Name);

            FText NewText = MakeText(Shown);
            memcpy((void*)&Line, (const void*)&NewText, sizeof(FText));

            Key = *(const void* const*)&Line;
            Note = L" (the game's own line was \"" + Native + L"\"" + (Before != Native ? L", \"" + Before + L"\" with the name put right" : L"") +
                   L" -- swapped: nobody got this elim)";

            GNoKillerDeaths.erase(GNoKillerDeaths.begin() + (ptrdiff_t)d);
            bSwapped = true;
            break;
        }

        StillThere.push_back(Key);
        TellServer(L"killfeed line on this screen: \"" + Shown + L"\"" + Note);

        if (LinesSeen % 25 == 0)
            ReportPlayerIds(L"so far");
    }

    GKillFeedSeen.swap(StillThere);

    for (size_t d = 0; d < GNoKillerDeaths.size();)
    {
        if (Now - GNoKillerDeaths[d].At > 4000)
        {
            TellServer(L"killfeed: no line naming '" + GNoKillerDeaths[d].Name + L"' turned up in 4s, so no \"no killer\" line could go in.");
            GNoKillerDeaths.erase(GNoKillerDeaths.begin() + (ptrdiff_t)d);
        }
        else
        {
            d++;
        }
    }

    return bSwapped;
}

static void RefreshKillFeedWidget()
{
    if (GKillFeedWidget && GKillFeedWidgetIndex >= 0)
    {
        auto Item = TUObjectArray::GetItemByIndex(GKillFeedWidgetIndex);

        if (!Item || Item->Object != GKillFeedWidget)
            GKillFeedWidget = nullptr;
    }

    if (!GKillFeedWidget)
    {
        GKillFeedWidget = (UObject*)TUObjectArray::FindFirstObject("AthenaKillFeedWidget_C");
        GKillFeedWidgetIndex = GKillFeedWidget ? GKillFeedWidget->Index : -1;
    }

    if (!GKillFeedWidget)
        return;

    if (auto Fn = GKillFeedWidget->GetFunction("UpdateKillFeed"))
        GKillFeedWidget->ProcessEvent(Fn, nullptr);
}

static void OnClientReportKill(UFunction* Function, void* Params)
{
    static int Kind = -1;
    static uint32 Offset = 0;

    if (Kind == -1)
    {
        Kind = 2;
        auto Named = Function->GetParamsNamed();

        if (!Named.NameOffsetMap.empty())
        {
            Offset = Named.NameOffsetMap[0].Offset;
            Kind = Named.NameOffsetMap[0].ElementSize == 0x10 ? 0 : (Named.NameOffsetMap[0].ElementSize == 8 ? 1 : 2);
        }
    }

    if (!Params || Kind == 2)
        return;

    std::wstring Name;

    if (Kind == 0)
    {
        auto& Given = *(FString*)((uint8*)Params + Offset);

        if (Given.CStr() && Given.Num() > 1)
            Name = std::wstring(Given.CStr());
    }
    else
    {
        Name = PlayerNameOf(*(UObject**)((uint8*)Params + Offset));
    }

    TellServer(Name.empty() ? std::wstring(L"popup on this screen: \"Eliminated\" with NO name")
                            : (L"popup on this screen: \"Eliminated " + Name + L"\""));
}

static void ReportStorm()
{
    struct FSeen
    {
        const void* Indicator = (const void*)1;
        int Phase = -999;
        float StartsAt = -12345.f, Start = -12345.f, Finish = -12345.f, LastR = -12345.f, NextR = -12345.f;
    };

    static FSeen Seen{};
    static UFunction* ServerTimeFn = nullptr;
    static bool bLooked = false;
    static int32 PhaseOffset = -2, StartsAtOffset = -2, IndicatorOffset = -2;
    static int32 StartOffset = -2, FinishOffset = -2, RadiusOffset = -2, LastROffset = -2, NextROffset = -2;

    auto World = UWorld::GetWorld();
    if (!World || !World->GameState)
        return;

    auto GameState = (UObject*)World->GameState;

    static const void* LookedClass = nullptr;

    if (!bLooked || (const void*)GameState->Class != LookedClass)
    {
        bLooked = true;
        LookedClass = (const void*)GameState->Class;
        ServerTimeFn = GameState->GetFunction("GetServerWorldTimeSeconds");
        PhaseOffset = ReflectedOffset(GameState, "SafeZonePhase");
        StartsAtOffset = ReflectedOffset(GameState, "SafeZonesStartTime");
        IndicatorOffset = ReflectedOffset(GameState, "SafeZoneIndicator");
        Seen = FSeen{};
    }

    if (IndicatorOffset < 0)
        return;

    FSeen Now{};
    auto Indicator = GetFromOffset<UObject*>(GameState, (uint32)IndicatorOffset);

    Now.Indicator = (const void*)Indicator;
    Now.Phase = PhaseOffset >= 0 ? (int)GetFromOffset<uint8>(GameState, (uint32)PhaseOffset) : -1;
    Now.StartsAt = StartsAtOffset >= 0 ? GetFromOffset<float>(GameState, (uint32)StartsAtOffset) : -1.f;

    float Radius = -1.f;

    if (Indicator)
    {
        if (StartOffset == -2)
        {
            StartOffset = ReflectedOffset(Indicator, "SafeZoneStartShrinkTime");
            FinishOffset = ReflectedOffset(Indicator, "SafeZoneFinishShrinkTime");
            RadiusOffset = ReflectedOffset(Indicator, "Radius");
            LastROffset = ReflectedOffset(Indicator, "LastRadius");
            NextROffset = ReflectedOffset(Indicator, "NextRadius");
        }

        Now.Start = StartOffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)StartOffset) : -1.f;
        Now.Finish = FinishOffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)FinishOffset) : -1.f;
        Now.LastR = LastROffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)LastROffset) : -1.f;
        Now.NextR = NextROffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)NextROffset) : -1.f;
        Radius = RadiusOffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)RadiusOffset) : -1.f;
    }

    auto Moved = [](float A, float B, float Tolerance) { return fabsf(A - B) > Tolerance; };

    if (Now.Indicator == Seen.Indicator && Now.Phase == Seen.Phase && !Moved(Now.StartsAt, Seen.StartsAt, 0.25f) &&
        !Moved(Now.Start, Seen.Start, 0.25f) && !Moved(Now.Finish, Seen.Finish, 0.25f) && !Moved(Now.LastR, Seen.LastR, 1.f) &&
        !Moved(Now.NextR, Seen.NextR, 1.f))
        return;

    Seen = Now;

    const float ServerNow = ServerTimeFn ? GameState->Call<float>(ServerTimeFn) : -1.f;

    auto Secs = [](float Value) { return OneDecimal((double)Value); };

    if (!Indicator)
    {
        TellServer(L"storm timer on this screen: no storm yet | phase " + std::to_wstring(Now.Phase) + L" | storm start time " +
                   Secs(Now.StartsAt) + L" (server time now " + Secs(ServerNow) + L", " +
                   (Now.StartsAt >= ServerNow ? L"in " + Secs(Now.StartsAt - ServerNow) : Secs(ServerNow - Now.StartsAt) + L" ago") + L"s)");
        return;
    }

    const wchar_t* What = ServerNow < Now.Start ? L"holding" : (ServerNow < Now.Finish ? L"CLOSING" : L"done, due to move on");

    TellServer(L"storm timer on this screen: phase " + std::to_wstring(Now.Phase) + L", " + What + L" | starts closing in " +
               Secs(Now.Start - ServerNow) + L"s, closed in " + Secs(Now.Finish - ServerNow) + L"s | radius " + Secs(Radius) +
               L", " + Secs(Now.LastR) + L" -> " + Secs(Now.NextR) + L" | server time " + Secs(ServerNow));
}

static bool GGliderReportBroken = false;

static int ReadBoolByName(const UObject* Object, const char* Name)
{
    if (!Object)
        return -1;

    auto Prop = Object->GetProperty(Name, 0x20000);
    if (!Prop)
        return -1;

    const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
    const uint8 Mask = Prop->GetFieldMask();

    return (GetFromOffset<uint8>(Object, Offset) & (Mask ? Mask : (uint8)0xFF)) ? 1 : 0;
}

static std::wstring WideObjectName(const UObject* Object)
{
    if (!Object)
        return std::wstring(L"NONE");

    const auto Narrow = Object->Name.ToString();
    return std::wstring(Narrow.begin(), Narrow.end());
}

static std::wstring GliderMeshOnScreen(const UObject* Pawn, const char* ComponentName, const char* MeshName, bool* bShowing)
{
    const int32 Offset = ReflectedOffset(Pawn, ComponentName);
    if (Offset < 0)
        return std::wstring(L"-");

    auto Component = GetFromOffset<UObject*>(Pawn, (uint32)Offset);
    if (!Component)
        return std::wstring(L"no component");

    const int32 MeshOffset = ReflectedOffset(Component, MeshName);
    auto Mesh = MeshOffset >= 0 ? GetFromOffset<UObject*>(Component, (uint32)MeshOffset) : nullptr;

    const int Visible = ReadBoolByName(Component, "bVisible");
    const int Hidden = ReadBoolByName(Component, "bHiddenInGame");

    if (Mesh && Visible == 1 && Hidden != 1)
        *bShowing = true;

    return WideObjectName(Mesh) + (Visible == 1 ? (Hidden == 1 ? L" (hidden in game)" : L" (showing)") : L" (not showing)");
}

static void ReportGliders(ULONGLONG Now)
{
    struct FSeen
    {
        float FirstZ = 0.f;
        ULONGLONG FirstAt = 0;
        bool bChecked = false;
    };

    static const UClass* PawnClass = nullptr;
    static bool bDone = false;
    static ULONGLONG FirstScan = 0, LastOpenAt = 0;
    static std::vector<std::pair<const void*, FSeen>> Seen;
    static int Players = 0, Checked = 0, WithGlider = 0, WithMesh = 0, Lines = 0;
    static bool bYouChecked = false;
    static int32 LoadoutOffset = -2, GliderOffset = -2;

    if (bDone)
        return;

    if (FirstScan == 0)
        FirstScan = Now;

    if (!PawnClass)
    {
        PawnClass = FindClass("FortPlayerPawnAthena");

        if (!PawnClass)
        {
            bDone = true;
            TellServer(L"gliders on this screen: this build has no FortPlayerPawnAthena -- not watched.");
            return;
        }
    }

    if (Players == 0 && Now - FirstScan > 600000)
    {
        bDone = true;
        TellServer(L"gliders on this screen: nobody seen under an open glider in 10 minutes -- stopped looking.");
        return;
    }

    auto Local = LocalController();
    const int32 LocalStateOffset = ReflectedOffset(Local, "PlayerState");
    const UObject* LocalState = LocalStateOffset >= 0 ? GetFromOffset<UObject*>(Local, (uint32)LocalStateOffset) : nullptr;

    bool bAnyOpen = false;

    for (int i = 0; i < TUObjectArray::Num(); i++)
    {
        auto Item = TUObjectArray::GetItemByIndex(i);
        if (!Item || !Item->Object || (Item->Flags & (0x20000000 | 0x10000000)))
            continue;

        const UObject* Pawn = Item->Object;
        if (Pawn->IsDefaultObject() || !Pawn->IsA(PawnClass))
            continue;

        if (ReadBoolByName(Pawn, "bIsParachuteOpen") != 1)
            continue;

        bAnyOpen = true;
        LastOpenAt = Now;

        const float Z = (float)((AActor*)Pawn)->K2_GetActorLocation().Z;

        FSeen* Mine = nullptr;
        for (auto& Entry : Seen)
            if (Entry.first == (const void*)Pawn)
                Mine = &Entry.second;

        if (!Mine)
        {
            FSeen First{};
            First.FirstZ = Z;
            First.FirstAt = Now;
            Seen.push_back({ (const void*)Pawn, First });
            Players++;
            continue;
        }

        if (Mine->bChecked || Now - Mine->FirstAt < 3000)
            continue;

        Mine->bChecked = true;
        Checked++;

        if (LoadoutOffset == -2)
        {
            LoadoutOffset = ReflectedOffset(Pawn, "CustomizationLoadout");

            if (LoadoutOffset < 0)
                LoadoutOffset = ReflectedOffset(Pawn, "CosmeticLoadout");

            auto Struct = FindStruct("FortAthenaLoadout");
            const uint32 Off = Struct ? Struct->GetOffset("Glider") : (uint32)-1;
            GliderOffset = (Off == (uint32)-1 || Off > 0x200) ? -1 : (int32)Off;
        }

        const UObject* Glider = (LoadoutOffset >= 0 && GliderOffset >= 0) ? GetFromOffset<UObject*>(Pawn, (uint32)(LoadoutOffset + GliderOffset))
                                                                           : nullptr;
        bool bShowing = false;
        const std::wstring Skeletal = GliderMeshOnScreen(Pawn, "MeshParachuteOpen", "SkeletalMesh", &bShowing);
        const std::wstring Static = GliderMeshOnScreen(Pawn, "ParachuteMeshOpen", "StaticMesh", &bShowing);

        std::wstring Attachment;
        const int32 AttachmentOffset = ReflectedOffset(Pawn, "ParachuteAttachment");

        if (AttachmentOffset >= 0)
        {
            auto GliderActor = GetFromOffset<UObject*>(Pawn, (uint32)AttachmentOffset);

            if (GliderActor)
            {
                const std::wstring ActorMesh = GliderMeshOnScreen(GliderActor, "ParachuteMesh", "SkeletalMesh", &bShowing);
                Attachment = L" | glider actor: " + WideObjectName(GliderActor) + L", its mesh: " + ActorMesh;
            }
            else
            {
                Attachment = L" | glider actor: NONE";
            }
        }

        if (Glider)
            WithGlider++;
        if (bShowing)
            WithMesh++;

        const int32 StateOffset = ReflectedOffset(Pawn, "PlayerState");
        const UObject* State = StateOffset >= 0 ? GetFromOffset<UObject*>(Pawn, (uint32)StateOffset) : nullptr;
        const bool bYou = State && State == LocalState;

        if (bYou)
            bYouChecked = true;

        if (!bYou && ++Lines > 8)
            continue;

        const std::wstring Name = PlayerNameOf(State);
        const double Seconds = (double)(Now - Mine->FirstAt) / 1000.0;

        TellServer(L"glider on this screen: '" + (Name.empty() ? std::wstring(L"?") : Name) + L"'" +
                   std::wstring(bYou ? L" (you)" : L"") + L" -- glider in their loadout: " + WideObjectName(Glider) +
                   L" | glider mesh: " + Skeletal + L", umbrella/static mesh: " + Static + Attachment + L" | still marked skydiving: " +
                   (ReadBoolByName(Pawn, "bIsSkydiving") == 1 ? L"yes" : L"no") + L" | came down " +
                   OneDecimal((double)(Mine->FirstZ - Z) / (Seconds > 0.0 ? Seconds : 1.0)) + L" a second in the " + OneDecimal(Seconds) +
                   L"s after it opened");
    }

    if (!bAnyOpen && Players > 0 && ((bYouChecked && Now - LastOpenAt >= 20000) || Now - LastOpenAt >= 120000))
    {
        bDone = true;
        TellServer(L"gliders on this screen, the drop is over: " + std::to_wstring(Players) + L" player(s) seen under an open glider | " +
                   std::to_wstring(Checked) + L" checked 3s after it opened: " + std::to_wstring(WithGlider) + L" with a glider in their loadout, " +
                   std::to_wstring(WithMesh) + L" with a glider mesh showing.");
    }
}

static int ReportGlidersGuarded(ULONGLONG Now)
{
    __try
    {
        ReportGliders(Now);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static bool GBotReportBroken = false;

static std::wstring ComponentMeshOnScreen(const UObject* Component, bool* bShowing)
{
    if (!Component)
        return std::wstring(L"no component");

    const int32 MeshOffset = ReflectedOffset(Component, "SkeletalMesh");
    auto Mesh = MeshOffset >= 0 ? GetFromOffset<UObject*>(Component, (uint32)MeshOffset) : nullptr;

    const int Visible = ReadBoolByName(Component, "bVisible");
    const int Hidden = ReadBoolByName(Component, "bHiddenInGame");

    if (Mesh && Visible == 1 && Hidden != 1)
        *bShowing = true;

    return WideObjectName(Mesh) + (Visible == 1 ? (Hidden == 1 ? L" (hidden in game)" : L" (showing)") : L" (not showing)");
}

static std::wstring PartSentFor(const UObject* PlayerState, int Slot)
{
    static int32 PartsOffset = -2;

    if (!PlayerState)
        return std::wstring(L"?");

    if (PartsOffset == -2)
    {
        const int32 Outer = ReflectedOffset(PlayerState, "CharacterParts");
        auto PartsStruct = FindStruct("CustomCharacterParts");
        const uint32 Inner = PartsStruct ? PartsStruct->GetOffset("Parts") : (uint32)-1;

        PartsOffset = Outer < 0 ? -1 : Outer + ((Inner == (uint32)-1 || Inner >= 0x40) ? 0 : (int32)Inner);
    }

    if (PartsOffset < 0)
        return std::wstring(L"-");

    return WideObjectName(GetFromOffset<UObject*>(PlayerState, (uint32)(PartsOffset + Slot * (int32)sizeof(void*))));
}

static void ReportBots(ULONGLONG Now)
{
    struct FSeen
    {
        ULONGLONG GroundSince = 0;
        bool bChecked = false;
        bool bWasInTheAir = false;
    };

    static const UClass* PawnClass = nullptr;
    static bool bDone = false;
    static ULONGLONG FirstCheck = 0;
    static std::vector<std::pair<const void*, FSeen>> Seen;
    static int Checked = 0, WithHead = 0, WithBody = 0, WithGun = 0, GunShowing = 0, Holstered = 0, Lines = 0;

    if (bDone)
        return;

    if (!PawnClass)
    {
        PawnClass = FindClass("FortPlayerPawnAthena");

        if (!PawnClass)
        {
            bDone = true;
            return;
        }
    }

    for (int i = 0; i < TUObjectArray::Num(); i++)
    {
        auto Item = TUObjectArray::GetItemByIndex(i);
        if (!Item || !Item->Object || (Item->Flags & (0x20000000 | 0x10000000)))
            continue;

        const UObject* Pawn = Item->Object;
        if (Pawn->IsDefaultObject() || !Pawn->IsA(PawnClass))
            continue;

        const int32 StateOffset = ReflectedOffset(Pawn, "PlayerState");
        const UObject* State = StateOffset >= 0 ? GetFromOffset<UObject*>(Pawn, (uint32)StateOffset) : nullptr;

        if (!State || ReadBoolByName(State, "bIsABot") != 1)
            continue;

        FSeen* Mine = nullptr;
        for (auto& Entry : Seen)
            if (Entry.first == (const void*)Pawn)
                Mine = &Entry.second;

        if (!Mine)
        {
            Seen.push_back({ (const void*)Pawn, FSeen{} });
            Mine = &Seen.back().second;
        }

        if (Mine->bChecked)
            continue;

        const bool bInTheAir = ReadBoolByName(Pawn, "bIsSkydiving") == 1 || ReadBoolByName(Pawn, "bIsParachuteOpen") == 1;

        if (bInTheAir)
        {
            Mine->bWasInTheAir = true;
            Mine->GroundSince = 0;
            continue;
        }

        if (!Mine->bWasInTheAir)
            continue;

        if (Mine->GroundSince == 0)
        {
            Mine->GroundSince = Now;
            continue;
        }

        if (Now - Mine->GroundSince < 6000)
            continue;

        Mine->bChecked = true;
        Checked++;

        if (FirstCheck == 0)
            FirstCheck = Now;

        bool bHead = false, bBody = false;
        std::wstring Head = L"-", Body = L"-";
        const int32 PartsOffset = ReflectedOffset(Pawn, "CharacterPartSkeletalMeshComponents");

        if (PartsOffset >= 0)
        {
            Head = ComponentMeshOnScreen(GetFromOffset<UObject*>(Pawn, (uint32)PartsOffset), &bHead);
            Body = ComponentMeshOnScreen(GetFromOffset<UObject*>(Pawn, (uint32)(PartsOffset + (int32)sizeof(void*))), &bBody);
        }

        if (bHead)
            WithHead++;
        if (bBody)
            WithBody++;

        std::wstring Gun = L"nothing";
        const int32 WeaponOffset = ReflectedOffset(Pawn, "CurrentWeapon");
        const UObject* Weapon = WeaponOffset >= 0 ? GetFromOffset<UObject*>(Pawn, (uint32)WeaponOffset) : nullptr;
        bool bGunShowing = false;

        if (Weapon)
        {
            WithGun++;

            const int32 DataOffset = ReflectedOffset(Weapon, "WeaponData");
            const UObject* Data = DataOffset >= 0 ? GetFromOffset<UObject*>(Weapon, (uint32)DataOffset) : nullptr;

            const int32 MeshOffset = ReflectedOffset(Weapon, "WeaponMesh");
            const UObject* MeshComponent = MeshOffset >= 0 ? GetFromOffset<UObject*>(Weapon, (uint32)MeshOffset) : nullptr;

            const std::wstring Mesh = ComponentMeshOnScreen(MeshComponent, &bGunShowing);
            const bool bActorHidden = ReadBoolByName(Weapon, "bHidden") == 1;

            if (bActorHidden)
                bGunShowing = false;

            if (bGunShowing)
                GunShowing++;

            Gun = WideObjectName(Data) + L" (gun actor " + WideObjectName(Weapon) + (bActorHidden ? L", hidden" : L"") + L", its mesh: " + Mesh + L")";
        }

        const int HolsteredNow = ReadBoolByName(Pawn, "bWeaponHolstered");

        if (HolsteredNow == 1)
            Holstered++;

        if (++Lines > 8)
            continue;

        const std::wstring Name = PlayerNameOf(State);

        TellServer(L"bot on this screen: '" + (Name.empty() ? std::wstring(L"?") : Name) + L"' (6s after it landed) -- head: " + Head +
                   L", body: " + Body + L" | parts the game sent: head " + PartSentFor(State, 0) + L", body " + PartSentFor(State, 1) +
                   L" | in its hands: " + Gun + L" | holstered: " + (HolsteredNow == 1 ? L"YES" : (HolsteredNow == 0 ? L"no" : L"?")));
    }

    if (Checked > 0 && (Checked >= 30 || Now - FirstCheck >= 180000))
    {
        bDone = true;
        TellServer(L"bots on this screen, 6s after they landed: " + std::to_wstring(Checked) + L" checked | head showing: " +
                   std::to_wstring(WithHead) + L", body showing: " + std::to_wstring(WithBody) + L" | a gun in their hands: " +
                   std::to_wstring(WithGun) + L" (its mesh showing: " + std::to_wstring(GunShowing) + L") | holding nothing: " +
                   std::to_wstring(Checked - WithGun) + L" | gun holstered: " + std::to_wstring(Holstered));
    }
}

static int ReportBotsGuarded(ULONGLONG Now)
{
    __try
    {
        ReportBots(Now);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static bool GCueCountBroken = false;
static int GCueMineAirHits = 0;
static int GCueMineGroundHits = 0;
static int GCueMineOther = 0;
static int GCueBotShots = 0;
static int GCueBotShotsOnYou = 0;
static int GCueBotShotsOnBots = 0;
static int GCueBotShotsMissed = 0;
static int GCueFromOthers = 0;
static int GCueSamplesMine = 0;
static int GCueSamplesBots = 0;
static bool GCueFromBot = false;
static bool GBotShotFxOff = false;
static bool GBotShotFxFaultToReport = false;

static int BotAirState(const UObject* Pawn)
{
    if (!Pawn)
        return -1;

    const int32 StateOffset = ReflectedOffset(Pawn, "PlayerState");
    const UObject* State = StateOffset >= 0 ? GetFromOffset<UObject*>(Pawn, (uint32)StateOffset) : nullptr;

    if (!State || ReadBoolByName(State, "bIsABot") != 1)
        return -1;

    return (ReadBoolByName(Pawn, "bIsSkydiving") == 1 || ReadBoolByName(Pawn, "bIsParachuteOpen") == 1) ? 1 : 0;
}

static const UObject* StateOfPawn(const UObject* Pawn)
{
    if (!Pawn)
        return nullptr;

    const int32 StateOffset = ReflectedOffset(Pawn, "PlayerState");
    return StateOffset >= 0 ? GetFromOffset<UObject*>(Pawn, (uint32)StateOffset) : nullptr;
}

static const UObject* LocalPlayerState()
{
    auto Local = LocalController();
    const int32 Offset = ReflectedOffset(Local, "PlayerState");
    return Offset >= 0 ? GetFromOffset<UObject*>(Local, (uint32)Offset) : nullptr;
}

static std::wstring CueActorName(const UObject* Actor, const UObject* LocalState)
{
    if (!Actor)
        return L"nothing";

    const UObject* State = StateOfPawn(Actor);

    if (State && State == LocalState)
        return L"you";

    const std::wstring Name = State ? PlayerNameOf(State) : std::wstring();

    if (!Name.empty())
        return L"'" + Name + L"'" + std::wstring(ReadBoolByName(State, "bIsABot") == 1 ? L" (a bot)" : L"");

    return WideObjectName(Actor);
}

static void SampleDamageCue(const std::wstring& From, UFunction* Function, void* Params, const UObject* HitActor, const UObject* LocalState)
{
    static int32 SharedAt = -2, Location = -1, Magnitude = -1, Activate = -1, Fatal = -1, Shield = -1;

    if (SharedAt == -2)
    {
        SharedAt = -1;

        auto Named = Function->GetParamsNamed();

        for (auto& Param : Named.NameOffsetMap)
        {
            if (Param.Name == "SharedData")
                SharedAt = (int32)Param.Offset;
        }

        auto Struct = FindStruct("AthenaBatchedDamageGameplayCues_Shared");

        auto Field = [&](const char* Name) -> int32
        {
            const uint32 Off = Struct ? Struct->GetOffset(Name) : (uint32)-1;
            return (Off == (uint32)-1 || Off > 0x40) ? -1 : (int32)Off;
        };

        Location = Field("Location");
        Magnitude = Field("Magnitude");
        Activate = Field("bWeaponActivate");
        Fatal = Field("bIsFatal");
        Shield = Field("bIsShield");
    }

    if (SharedAt < 0 || !Params)
        return;

    const uint8* Shared = (const uint8*)Params + SharedAt;

    auto Flag = [&](int32 Offset) -> std::wstring
    {
        if (Offset < 0)
            return L"?";

        return Shared[Offset] ? L"yes" : L"no";
    };

    std::wstring Where = L"?";

    if (Location >= 0)
    {
        const bool bDoubles = FVector::Size() >= 0x18;
        const double X = bDoubles ? ((const double*)(Shared + Location))[0] : (double)((const float*)(Shared + Location))[0];
        const double Y = bDoubles ? ((const double*)(Shared + Location))[1] : (double)((const float*)(Shared + Location))[1];
        const double Z = bDoubles ? ((const double*)(Shared + Location))[2] : (double)((const float*)(Shared + Location))[2];

        Where = L"(" + OneDecimal(X) + L", " + OneDecimal(Y) + L", " + OneDecimal(Z) + L")";
    }

    const std::wstring Damage = Magnitude >= 0 ? OneDecimal((double)*(const float*)(Shared + Magnitude)) : std::wstring(L"?");

    TellServer(L"damage cue sample on this screen, fired by " + From + L": hit " + CueActorName(HitActor, LocalState) + L" | damage " + Damage +
               L" | gun-fired flag " + Flag(Activate) + L" | fatal " + Flag(Fatal) + L" | shield " + Flag(Shield) + L" | at " + Where);
}

static void CountBotDamageCue(UObject* Object, UFunction* Function, void* Params)
{
    static int32 NonSharedOffset = -2;

    if (NonSharedOffset == -2)
    {
        NonSharedOffset = -1;
        auto Named = Function->GetParamsNamed();

        for (auto& Param : Named.NameOffsetMap)
        {
            if (Param.Name == "NonSharedData")
                NonSharedOffset = (int32)Param.Offset;
        }

        if (NonSharedOffset < 0 || NonSharedOffset + 0x10 > (int32)Named.Size)
        {
            NonSharedOffset = -1;
            GCueCountBroken = true;
            TellServer(L"damage feedback on this screen: this build's NetMulticast_Athena_BatchedDamageCues does not look the way it "
                       L"should -- the damage cues are not counted.");
            return;
        }
    }

    if (NonSharedOffset < 0 || !Params)
        return;

    auto HitActor = *(const UObject**)((uint8*)Params + NonSharedOffset);

    const UObject* LocalState = LocalPlayerState();
    const UObject* SenderState = StateOfPawn(Object);

    if (SenderState && SenderState == LocalState)
    {
        const int AsBot = BotAirState(HitActor);

        if (AsBot == 1)
            GCueMineAirHits++;
        else if (AsBot == 0)
            GCueMineGroundHits++;
        else
            GCueMineOther++;

        if (++GCueSamplesMine <= 3)
            SampleDamageCue(L"you", Function, Params, HitActor, LocalState);

        return;
    }

    if (SenderState && ReadBoolByName(SenderState, "bIsABot") == 1)
    {
        GCueBotShots++;

        const UObject* HitState = StateOfPawn(HitActor);

        if (!HitActor)
            GCueBotShotsMissed++;
        else if (HitState && HitState == LocalState)
            GCueBotShotsOnYou++;
        else if (HitState && ReadBoolByName(HitState, "bIsABot") == 1)
            GCueBotShotsOnBots++;

        if (++GCueSamplesBots <= 3)
        {
            const std::wstring Name = PlayerNameOf(SenderState);
            SampleDamageCue(L"the bot '" + (Name.empty() ? std::wstring(L"?") : Name) + L"'", Function, Params, HitActor, LocalState);
        }

        return;
    }

    GCueFromOthers++;
}

static int CountBotDamageCueGuarded(UObject* Object, UFunction* Function, void* Params)
{
    __try
    {
        CountBotDamageCue(Object, Function, Params);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static int SenderIsBot(const UObject* Pawn)
{
    const UObject* State = StateOfPawn(Pawn);
    return (State && ReadBoolByName(State, "bIsABot") == 1) ? 1 : 0;
}

static int SenderIsBotGuarded(const UObject* Pawn)
{
    __try
    {
        return SenderIsBot(Pawn);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static int RunBotShotCueGuarded(UObject* Object, UFunction* Function, void* Params)
{
    __try
    {
        ProcessEventOG(Object, Function, Params);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static void ReportBotDamageCues()
{
    static int LastTotal = 0;

    const int Total = GCueMineAirHits + GCueMineGroundHits + GCueMineOther + GCueBotShots + GCueFromOthers;

    if (Total == LastTotal)
        return;

    LastTotal = Total;

    TellServer(L"damage cues on this screen (the game's hit cue -- your damage numbers come from it): fired by you -- hits on bots in the air " +
               std::to_wstring(GCueMineAirHits) + L", on the ground " + std::to_wstring(GCueMineGroundHits) + L", on anything else " +
               std::to_wstring(GCueMineOther) + L" | fired by bots: " + std::to_wstring(GCueBotShots) + L" (on you " +
               std::to_wstring(GCueBotShotsOnYou) + L", on other bots " + std::to_wstring(GCueBotShotsOnBots) +
               L", missed or hit something this screen does not have " + std::to_wstring(GCueBotShotsMissed) + L") | from anything else " +
               std::to_wstring(GCueFromOthers) +
               std::wstring(GBotShotFxOff ? L" | bot shot effects switched off on this screen after a fault" : L""));
}

static void BeforeEvent(UObject* Object, UFunction* Function, void* Params)
{
    const int32 Name = Function->Name.ComparisonIndex;

    if (Name == GNameBatchedDamageCues.ComparisonIndex)
    {
        GCueFromBot = SenderIsBotGuarded(Object) == 1;

        if (!GCueCountBroken && CountBotDamageCueGuarded(Object, Function, Params))
        {
            GCueCountBroken = true;
            TellServer(L"damage feedback on this screen: counting the damage cues faulted -- that count is switched off (nothing else is).");
        }

        return;
    }

    if (Name == GNameOnRepDeathInfo.ComparisonIndex)
    {
        if (!GKillFeedBroken)
        {
            OnDeathInfoArrived(Object);
            ExpectKillFeedLine(Object, true);
        }
    }
    else if (Name == GNameOnRepThankedBusDriver.ComparisonIndex)
    {
        if (!GKillFeedBroken)
            ExpectKillFeedLine(Object, false);
    }
    else if (Name == GNameUpdateKillFeed.ComparisonIndex)
    {

        GKillFeedWidget = Object;
        GKillFeedWidgetIndex = Object ? Object->Index : -1;

        if (!GKillFeedBroken)
            LookAtTheKillFeed();
    }
    else if (Name == GNameClientReportKill.ComparisonIndex)
    {
        OnClientReportKill(Function, Params);
    }
}

static void GameThreadTick()
{
    const ULONGLONG Now = GetTickCount64();

    static ULONGLONG LastKillFeed = 0, LastStorm = 0, LastGliders = 0, LastBots = 0, LastCues = 0;

    if (GFaultToReport)
    {
        GFaultToReport = false;
        TellServer(L"the client DLL's code hit a fault -- the killfeed lines are switched off for the rest of this game" +
                   std::wstring(GStormReportBroken ? L" (and these reports too)" : L"") + L".");
    }

    if (GBotShotFxFaultToReport)
    {
        GBotShotFxFaultToReport = false;
        TellServer(L"bot shot effects: playing one on this screen faulted inside the game -- they are switched off on this screen (the bots "
                   L"still shoot and still deal damage).");
    }

    if (!GKillFeedBroken && !GNoKillerDeaths.empty() && Now - LastKillFeed >= 250)
    {
        LastKillFeed = Now;

        if (LookAtTheKillFeed())
            RefreshKillFeedWidget();
    }

    if (!GStormReportBroken && Now - LastStorm >= 1000)
    {
        LastStorm = Now;
        ReportStorm();
    }

    if (!GGliderReportBroken && !GReportsBroken && Now - LastGliders >= 1000)
    {
        LastGliders = Now;

        if (ReportGlidersGuarded(Now))
        {
            GGliderReportBroken = true;
            TellServer(L"gliders on this screen: the client DLL's glider check hit a fault -- that check is switched off (nothing else is).");
        }
    }

    if (!GBotReportBroken && !GReportsBroken && Now - LastBots >= 2000)
    {
        LastBots = Now;

        if (ReportBotsGuarded(Now))
        {
            GBotReportBroken = true;
            TellServer(L"bots on this screen: the client DLL's check of the bots' heads and guns hit a fault -- that check is switched off.");
        }
    }

    if (!GCueCountBroken && !GReportsBroken && Now - LastCues >= 15000)
    {
        LastCues = Now;
        ReportBotDamageCues();
    }
}

static int BeforeEventGuarded(UObject* Object, UFunction* Function, void* Params)
{
    __try
    {
        BeforeEvent(Object, Function, Params);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static int GameThreadTickGuarded()
{
    __try
    {
        GameThreadTick();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static void ProcessEventHook(UObject* Object, UFunction* Function, void* Params)
{
    const DWORD GameThread = GGameThreadId.load(std::memory_order_relaxed);

    if (GInOurCode || GFaults >= 3 || GameThread == 0 || !Function || GetCurrentThreadId() != GameThread)
        return ProcessEventOG(Object, Function, Params);

    GInOurCode++;
    const int BeforeFault = BeforeEventGuarded(Object, Function, Params);
    GInOurCode--;

    const bool bBotShotCue = GCueFromBot && Function->Name.ComparisonIndex == GNameBatchedDamageCues.ComparisonIndex;
    GCueFromBot = false;

    if (!bBotShotCue)
    {
        ProcessEventOG(Object, Function, Params);
    }
    else if (!GBotShotFxOff && RunBotShotCueGuarded(Object, Function, Params))
    {
        GBotShotFxOff = true;
        GBotShotFxFaultToReport = true;
    }

    {
        const int32 Ran = Function->Name.ComparisonIndex;

        if ((Ran == GNameOnRepDeathInfo.ComparisonIndex || Ran == GNameOnRepThankedBusDriver.ComparisonIndex) && !GKillFeedExpect.empty())
            GKillFeedExpect.pop_back();
    }

    GInOurCode++;
    const int TickFault = GameThreadTickGuarded();
    GInOurCode--;

    if (BeforeFault || TickFault)
    {

        GFaults++;
        GKillFeedBroken = true;

        if (GFaults == 1)
        {
            GFaultToReport = true;
        }
        else
        {
            GReportsBroken = true;
            GStormReportBroken = true;
            GFaultToReport = false;
        }
    }
}

static HWND GGameWindow = nullptr;
static WNDPROC GGameWindowProcOG = nullptr;
static UINT GInstallHookMessage = 0;
static std::atomic<bool> GHookInstalled{ false };

static void InstallProcessEventHookNow()
{
    auto DefaultObject = UObject::StaticClass() ? UObject::StaticClass()->GetDefaultObj() : nullptr;
    void* ProcessEventAddress = DefaultObject ? DefaultObject->Vft[Offsets::ProcessEventVft] : nullptr;

    if (ProcessEventAddress)
        Hooking::Hook(ProcessEventAddress, (void*)ProcessEventHook, ProcessEventOG);
}

static LRESULT CALLBACK GameWindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    if (GInstallHookMessage != 0 && Message == GInstallHookMessage)
    {
        if (!GHookInstalled.exchange(true))
            InstallProcessEventHookNow();

        return 0;
    }

    return CallWindowProcW(GGameWindowProcOG, Window, Message, WParam, LParam);
}

static void AskTheGameThreadForTheHook()
{
    if (GHookInstalled.load())
        return;

    if (!GGameWindow)
    {
        FGameWindow Found{};
        EnumWindows(FindGameWindow, (LPARAM)&Found);

        if (!Found.Window || !Found.Thread)
            return;

        GInstallHookMessage = RegisterWindowMessageW(L"Four_E_Zero_H 9.10 client: install the game-thread hook");

        if (!GInstallHookMessage)
            return;

        GGameWindowProcOG = (WNDPROC)GetWindowLongPtrW(Found.Window, GWLP_WNDPROC);

        if (!GGameWindowProcOG)
            return;

        SetWindowLongPtrW(Found.Window, GWLP_WNDPROC, (LONG_PTR)GameWindowProc);

        GGameWindow = Found.Window;
        GGameThreadId.store(Found.Thread);
    }

    PostMessageW(GGameWindow, GInstallHookMessage, 0, 0);
}

static void InstallGameThreadHook()
{
    GNameOnRepDeathInfo = FName(L"OnRep_DeathInfo");
    GNameOnRepThankedBusDriver = FName(L"OnRep_ThankedBusDriver");
    GNameUpdateKillFeed = FName(L"UpdateKillFeed");
    GNameClientReportKill = FName(L"ClientReportKill");
    GNameBatchedDamageCues = FName(L"NetMulticast_Athena_BatchedDamageCues");

}

void ClientThread()
{
    bool bPressed = false;

    DWORD LastJoinTick = 0;

    while (true)
    {
        if (UWorld::GetWorld() && UWorld::GetWorld()->OwningGameInstance)
        {
            auto& LocalPlayers = UWorld::GetWorld()->OwningGameInstance->LocalPlayers;

            if (LocalPlayers.Num() > 0)
            {
                auto PlayerController = (AFortPlayerControllerAthena*)LocalPlayers[0]->PlayerController;

                if (PlayerController && !PlayerController->CheatManager)
                {
                    PlayerController->CheatManager = (UFortCheatManager*)UGameplayStatics::SpawnObject(PlayerController->CheatClass.Get(), PlayerController);
                    PlayerController->CheatManager->ObjectFlags &= ~0x1000000;
                    TUObjectArray::GetItemByIndex(PlayerController->CheatManager->Index)->Flags &= ~0x4000000;
                }
            }
        }

        AskTheGameThreadForTheHook();

        if (!bPressed && GetAsyncKeyState(VK_F3))
        {
            bPressed = true;

            bEOREnabled ^= 1;
        }
        else if (!bPressed && GetAsyncKeyState(VK_F4))
        {
            bPressed = true;

            bDisablePreEdits ^= 1;
        }
        else if (!bPressed && GetAsyncKeyState(VK_F5))
        {
            bPressed = true;

            const DWORD Now = GetTickCount();

            if (LastJoinTick == 0 || Now - LastJoinTick > 3000)
            {
                LastJoinTick = Now;
                JoinLocalServer();
            }
        }
        else if (!GetAsyncKeyState(VK_F3) && !GetAsyncKeyState(VK_F4) && !GetAsyncKeyState(VK_F5))
            bPressed = false;

        Sleep(33);
    }
}

void Client::Init()
{
    UEngine::GetEngine()->GameViewport->ViewportConsole = UGameplayStatics::SpawnObject(UEngine::GetEngine()->ConsoleClass, UEngine::GetEngine()->GameViewport);

    if (VersionInfo.FortniteVersion >= 10 || FConfiguration::bForceRespawns)
    {
        auto PrimarySlot = uint8_t(EPlaylistUIExtensionSlot::StaticEnum() ? EPlaylistUIExtensionSlot::GetPrimary() : EUIExtensionSlot::GetPrimary());

        TArray<FUIExtension> ArenaExtensions, ShowdownExtensions;

        FUIExtension ArenaUIExtension{};
        ArenaUIExtension.Slot = PrimarySlot;
        if (VersionInfo.FortniteVersion < 23)
            ArenaUIExtension.WidgetClass.ObjectID.AssetPathName = FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD.ArenaScoringHUD_C");
        else
        {
            auto& PackageName = *(FName*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x8));
            auto& AssetName = *(FName*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0xC));
            auto& SubPathString = *(FString*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10));

            PackageName = FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD");
            AssetName = FName(L"ArenaScoringHUD_C");
            SubPathString = FString();
        }

        FUIExtension ShowdownUIExtension{};
        ShowdownUIExtension.Slot = PrimarySlot;
        if (VersionInfo.FortniteVersion < 23)
            ShowdownUIExtension.WidgetClass.ObjectID.AssetPathName = FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD.ShowdownScoringHUD_C");
        else
        {
            auto& PackageName = *(FName*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x8));
            auto& AssetName = *(FName*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0xC));
            auto& SubPathString = *(FString*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10));

            PackageName.ComparisonIndex = FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD").ComparisonIndex;
            AssetName.ComparisonIndex = FName(L"ShowdownScoringHUD_C").ComparisonIndex;
            SubPathString = FString();
        }

        auto PlaylistClass = FindClass("FortPlaylistAthena");

        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            auto Object = TUObjectArray::GetObjectByIndex(i);

            if (Object && Object->IsA((UClass*)PlaylistClass))
            {
                auto Playlist = (UFortPlaylistAthena*)Object;

                if (FConfiguration::bForceRespawns)
                {
                    if (Playlist->HasbRespawnInAir())
                        Playlist->bRespawnInAir = true;
                    if (Playlist->HasRespawnHeight())
                    {
                        Playlist->RespawnHeight.Curve.CurveTable = nullptr;
                        Playlist->RespawnHeight.Curve.RowName = FName();
                        Playlist->RespawnHeight.Value = 20000;
                    }
                    if (Playlist->HasRespawnTime())
                    {
                        Playlist->RespawnTime.Curve.CurveTable = nullptr;
                        Playlist->RespawnTime.Curve.RowName = FName();
                        Playlist->RespawnTime.Value = 3;
                    }
                    Playlist->RespawnType = 1;
                    if (Playlist->HasbAllowJoinInProgress())
                        Playlist->bAllowJoinInProgress = true;

                    if (Playlist->HasbForceCameraFadeOnRespawn())
                        Playlist->bForceCameraFadeOnRespawn = true;
                }
                if (VersionInfo.FortniteVersion >= 10)
                {
                    auto Name = Object->Name.ToString();
                    if (Name.contains("Showdown"))
                        Playlist->UIExtensions.Add(Name.contains("ShowdownAlt") ? ArenaUIExtension : ShowdownUIExtension);
                }
            }
        }
    }

    if (VersionInfo.FortniteVersion < 24.30)
    {
        auto EditModeInputComponent = Memcury::Scanner::FindStringRef<const wchar_t*>(L"EditModeInputComponent0");

        auto rdataSect = Memcury::PE::Section::GetSection(".rdata");
        for (int i = 1; i < 0x5000; i++)
        {
            auto Addr = (uint8_t*)EditModeInputComponent.Get() + i;

            if (*Addr == 0x48 && *(Addr + 1) == 0x8D)
            {
                auto Rel = Memcury::Scanner(Addr).RelativeOffset(3);

                if (rdataSect.isInSection(Rel.Get()) && strcmp((char*)Rel.Get(), "CompleteBuildingEditInteraction") == 0)
                {
                    CompleteBuildingEditInteraction = Memcury::Scanner(Addr).ScanFor({ 0x48, 0x8D }, false).RelativeOffset(3).GetAs<void (*)(void*)>();
                    break;
                }
            }
        }

        auto SelectEditScanner = EditModeInputComponent;
        SelectEditScanner.ScanFor({ 0x48, 0x8D, 0x05 }, true, 1);

        if (VersionInfo.FortniteVersion < 24.30)
        {
            auto SelectResetScanner = SelectEditScanner;
            auto SelectResetAddr = SelectResetScanner.ScanFor({ 0x48, 0x8D, 0x05 }, true).RelativeOffset(3).Get();

            Hooking::Hook(SelectResetAddr, SelectReset, SelectResetOG);
        }

        if (VersionInfo.FortniteVersion < 11)
        {
            auto SelectEditAddr = SelectEditScanner.RelativeOffset(3).Get();

            Hooking::Hook(SelectEditAddr, SelectEdit, SelectEditOG);
        }

        if (VersionInfo.FortniteVersion < 15.20)
        {
            auto rdataSect = Memcury::PE::Section::GetSection(".rdata");
            for (int i = 1; i < 0x5000; i++)
            {
                auto Addr = (uint8_t*)EditModeInputComponent.Get() - i;

                if (*Addr == 0x48 && *(Addr + 1) == 0x8D)
                {
                    auto Rel = Memcury::Scanner(Addr).RelativeOffset(3);

                    if (rdataSect.isInSection(Rel.Get()) && strcmp((char*)Rel.Get(), "PerformBuildingEditInteraction") == 0)
                    {
                        auto PerformBuildingEditInteractionAddr = Memcury::Scanner(Addr).ScanFor({ 0x48, 0x8D }, false).RelativeOffset(3).Get();

                        Hooking::Hook(PerformBuildingEditInteractionAddr, PerformBuildingEditInteraction, PerformBuildingEditInteractionOG);
                        break;
                    }
                }
            }
        }
    }

    InstallGameThreadHook();

    CreateThread(0, 0, (LPTHREAD_START_ROUTINE)ClientThread, 0, 0, 0);
}
