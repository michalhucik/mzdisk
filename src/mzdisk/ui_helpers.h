/**
 * @file   ui_helpers.h
 * @brief  ImGui pomocné funkce sdílené napříč panely (C++ only).
 */

#ifndef MZDISK_UI_HELPERS_H
#define MZDISK_UI_HELPERS_H

#include "libs/imgui/imgui.h"

/**
 * @brief Vykreslí tab item s explicitním tooltipem obsahujícím label.
 *
 * `ImGui::BeginTabItem` samo od sebe zobrazuje tooltip pouze když je
 * viditelný text oříznutý (clipped) - kratší taby tak tooltip nemají.
 * Ve vícejazyčné aplikaci to vytváří viditelnou nekonzistenci: některé
 * překlady se vejdou, jiné ne, a uživatel dostává tooltip jen na části
 * tabů.
 *
 * Tato obálka zařídí, aby tooltip měl KAŽDÝ tab - hovered tab vždy
 * ukáže svůj plný label bez ohledu na to, jestli je oříznutý.
 *
 * Rozhraní je identické s `ImGui::BeginTabItem`.
 *
 * @param[in]     label  Textový label tabu (už lokalizovaný přes `_()`).
 * @param[in,out] p_open Volitelný přepínač viditelnosti (klasický close button).
 * @param[in]     flags  ImGuiTabItemFlags.
 * @return true pokud je tab aktivní (kreslí se jeho obsah), jinak false.
 *
 * @note Stejně jako `BeginTabItem` je parný s `EndTabItem` jen když
 *       vrátí true.
 */
static inline bool TabItemWithTooltip ( const char *label, bool *p_open = nullptr,
                                         ImGuiTabItemFlags flags = 0 )
{
    bool active = ImGui::BeginTabItem ( label, p_open, flags );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) {
        ImGui::SetTooltip ( "%s", label );
    }
    return active;
}


/**
 * @brief Nastaví výchozí velikost a vystředěnou pozici okna pro první
 *        otevření, pokud není pozice v `imgui.ini` uložená.
 *
 * Řeší problém, kdy okna bez uloženého stavu (`imgui.ini`) vznikala
 * na výchozí pozici (0,0), která na některých setupech (multi-monitor,
 * DPI scaling) skončila mimo viditelnou oblast. Tento helper zajistí,
 * že okno se při první inkarnaci vystředí vůči hlavnímu viewportu.
 *
 * Používá `ImGuiCond_FirstUseEver` - po uložení pozice do .ini se už
 * další sezení neovlivní (uživatelovy zvyky přetrvají).
 *
 * Voláno PŘED `ImGui::Begin()`, `ImGuiFileDialog::Display()` apod.
 *
 * @param[in] w Výchozí šířka okna v pixelech.
 * @param[in] h Výchozí výška okna v pixelech.
 */
static inline void SetNextWindowDefaultCentered ( float w, float h )
{
    ImGuiViewport *vp = ImGui::GetMainViewport ();
    ImGui::SetNextWindowPos ( vp->GetCenter (),
                               ImGuiCond_FirstUseEver,
                               ImVec2 ( 0.5f, 0.5f ) );
    ImGui::SetNextWindowSize ( ImVec2 ( w, h ), ImGuiCond_FirstUseEver );
}


/**
 * @brief Tlačítko s pevnou minimální šířkou, které se ale umí rozrůst,
 *        pokud lokalizovaný text nevjde do minima.
 *
 * Standardní `ImGui::Button(label, ImVec2(120, 0))` zafixuje šířku 120
 * bez ohledu na text. V angličtině se krátké labely ("OK", "Cancel",
 * "Skip") vejdou, ale v češtině/němčině se dlouhé varianty
 * ("Přejmenovat", "Alles abbrechen") můžou oříznout. Tato obálka vezme
 * větší z `min_width` a reálné šířky textu + padding, takže tlačítko
 * se vždy vejde kolem svého textu a zároveň drží minimum pro konzistenci
 * s krátkými sousedy.
 *
 * @param[in] label      Text tlačítka (už lokalizovaný).
 * @param[in] min_width  Minimální šířka v pixelech (default 120).
 * @return true pokud bylo tlačítko stisknuto.
 */
static inline bool ButtonMinWidth ( const char *label, float min_width = 120.0f )
{
    ImGuiStyle &style = ImGui::GetStyle ();
    float text_w = ImGui::CalcTextSize ( label, nullptr, true ).x;
    float w = text_w + style.FramePadding.x * 2.0f;
    if ( w < min_width ) w = min_width;
    return ImGui::Button ( label, ImVec2 ( w, 0 ) );
}

#endif /* MZDISK_UI_HELPERS_H */
