/**
 * @file keyboard_text.c
 * @brief On-Screen Keyboard Text Layouts for Naming and Chat Screens
 *
 * FILE OVERVIEW:
 * This data-only file defines the text strings that form the visual layout of
 * on-screen keyboards used in three different game systems:
 *
 *   1. Easy Chat Keyboard — used in the Easy Chat system where players select
 *      predefined words/phrases (e.g., for setting Trainer greetings)
 *   2. Naming Screen Keyboard — used when the player names their character,
 *      Pokemon, or PC boxes (supports uppercase, lowercase, numbers, symbols)
 *   3. Union Room Chat Keyboard — used for free-text chat in the wireless
 *      Union Room feature, including letter, number, and emoji pages
 *
 * GBA CONTEXT:
 * Each keyboard row is stored as a string with {CLEAR N} control codes that
 * insert horizontal spacing between characters. This is how the game achieves
 * evenly-spaced keyboard layouts using the variable-width text rendering system.
 * The _() macro converts these into the GBA's custom character encoding.
 * Emoji entries like {EMOJI_HAPPY} are special control characters rendered as
 * small graphical icons rather than text glyphs.
 */
#include "global.h"

/* ========================================================================
 * EASY CHAT KEYBOARD — used for selecting predefined phrases
 * ======================================================================== */
const u8 gText_EasyChatKeyboard_ABCDEFothers[] = _("{CLEAR 11}A{CLEAR 6}B{CLEAR 6}C{CLEAR 26}D{CLEAR 6}E{CLEAR 6}F{CLEAR 26}others");
const u8 gText_EasyChatKeyboard_GHIJKL[] = _("{CLEAR 11}G{CLEAR 6}H{CLEAR 6}I{CLEAR 26}J{CLEAR 6}K{CLEAR 6}L");
const u8 gText_EasyChatKeyboard_MNOPQRS[] = _("{CLEAR 11}M{CLEAR 6}N{CLEAR 6}O{CLEAR 26}P{CLEAR 6}Q{CLEAR 6}R{CLEAR 6}S{CLEAR 26} ");
const u8 gText_EasyChatKeyboard_TUVWXYZ[] = _("{CLEAR 11}T{CLEAR 6}U{CLEAR 6}V{CLEAR 26}W{CLEAR 6}X{CLEAR 6}Y{CLEAR 6}Z{CLEAR 26} ");

// Naming Screen keyboard
const u8 gText_NamingScreenKeyboard_abcdef[] = _("{CLEAR 11}a{CLEAR 6}b{CLEAR 6}c{CLEAR 26}d{CLEAR 6}e{CLEAR 6}f{CLEAR 6} {CLEAR 26}.");
const u8 gText_NamingScreenKeyboard_ghijkl[] = _("{CLEAR 11}g{CLEAR 6}h{CLEAR 7}i{CLEAR 27}j{CLEAR 6}k{CLEAR 6}l{CLEAR 7} {CLEAR 26},");
const u8 gText_NamingScreenKeyboard_mnopqrs[] = _("{CLEAR 11}m{CLEAR 6}n{CLEAR 7}o{CLEAR 26}p{CLEAR 6}q{CLEAR 7}r{CLEAR 6}s{CLEAR 27} ");
const u8 gText_NamingScreenKeyboard_tuvwxyz[] = _("{CLEAR 12}t{CLEAR 6}u{CLEAR 6}v{CLEAR 26}w{CLEAR 6}x{CLEAR 6}y{CLEAR 6}z{CLEAR 26} ");
const u8 gText_NamingScreenKeyboard_ABCDEF[] = _("{CLEAR 11}A{CLEAR 6}B{CLEAR 6}C{CLEAR 26}D{CLEAR 6}E{CLEAR 6}F{CLEAR 6} {CLEAR 26}.");
const u8 gText_NamingScreenKeyboard_GHIJKL[] = _("{CLEAR 11}G{CLEAR 6}H{CLEAR 6}I{CLEAR 26}J{CLEAR 6}K{CLEAR 6}L{CLEAR 6} {CLEAR 26},");
const u8 gText_NamingScreenKeyboard_MNOPQRS[] = _("{CLEAR 11}M{CLEAR 6}N{CLEAR 6}O{CLEAR 26}P{CLEAR 6}Q{CLEAR 6}R{CLEAR 6}S{CLEAR 26} ");
const u8 gText_NamingScreenKeyboard_TUVWXYZ[] = _("{CLEAR 11}T{CLEAR 6}U{CLEAR 6}V{CLEAR 26}W{CLEAR 6}X{CLEAR 6}Y{CLEAR 6}Z{CLEAR 26} ");
const u8 gText_NamingScreenKeyboard_01234[] = _("{CLEAR 11}0{CLEAR 16}1{CLEAR 16}2{CLEAR 16}3{CLEAR 16}4{CLEAR 16} ");
const u8 gText_NamingScreenKeyboard_56789[] = _("{CLEAR 11}5{CLEAR 16}6{CLEAR 16}7{CLEAR 16}8{CLEAR 16}9{CLEAR 16} ");
const u8 gText_NamingScreenKeyboard_Symbols1[] = _("{CLEAR 11}!{CLEAR 16}?{CLEAR 16}♂{CLEAR 16}♀{CLEAR 16}/{CLEAR 16}-");
const u8 gText_NamingScreenKeyboard_Symbols2[] = _("{CLEAR 11}…{CLEAR 16}“{CLEAR 16}”{CLEAR 18}‘{CLEAR 18}'{CLEAR 18} ");

// Union Room Chat keyboard
const u8 gText_UnionRoomChatKeyboard_ABCDE[] = _("ABCDE");
const u8 gText_UnionRoomChatKeyboard_FGHIJ[] = _("FGHIJ");
const u8 gText_UnionRoomChatKeyboard_KLMNO[] = _("KLMNO");
const u8 gText_UnionRoomChatKeyboard_PQRST[] = _("PQRST");
const u8 gText_UnionRoomChatKeyboard_UVWXY[] = _("UVWXY");
const u8 gText_UnionRoomChatKeyboard_Z[] = _("Z    ");
const u8 gText_UnionRoomChatKeyboard_01234Upper[] = _("01234");
const u8 gText_UnionRoomChatKeyboard_56789Upper[] = _("56789");
const u8 gText_UnionRoomChatKeyboard_PunctuationUpper[] = _(".,!? ");
const u8 gText_UnionRoomChatKeyboard_SymbolsUpper[] = _("-/&… ");
const u8 gText_UnionRoomChatKeyboard_abcde[] = _("abcde");
const u8 gText_UnionRoomChatKeyboard_fghij[] = _("fghij");
const u8 gText_UnionRoomChatKeyboard_klmno[] = _("klmno");
const u8 gText_UnionRoomChatKeyboard_pqrst[] = _("pqrst");
const u8 gText_UnionRoomChatKeyboard_uvwxy[] = _("uvwxy");
const u8 gText_UnionRoomChatKeyboard_z[] = _("z    ");
const u8 gText_UnionRoomChatKeyboard_01234Lower[] = _("01234");
const u8 gText_UnionRoomChatKeyboard_56789Lower[] = _("56789");
const u8 gText_UnionRoomChatKeyboard_PunctuationLower[] = _(".,!? ");
const u8 gText_UnionRoomChatKeyboard_SymbolsLower[] = _("-/&… ");

const u8 gText_EmptyTextInput1[] = _("");
const u8 gText_EmptyTextInput2[] = _("");
const u8 gText_EmptyTextInput3[] = _("");
const u8 gText_EmptyTextInput4[] = _("");
const u8 gText_EmptyTextInput5[] = _("");
const u8 gText_EmptyTextInput6[] = _("");
const u8 gText_EmptyTextInput7[] = _("");
const u8 gText_EmptyTextInput8[] = _("");

// Union Room Chat keyboard emojis
const u8 gText_UnionRoomChatKeyboard_Emoji1[] = _("{EMOJI_MISCHIEVOUS}{EMOJI_HAPPY}{EMOJI_ANGRY}{EMOJI_SURPRISED}{EMOJI_BIGANGER}");
const u8 gText_UnionRoomChatKeyboard_Emoji2[] = _("{EMOJI_BIGSMILE}{EMOJI_EVIL}{EMOJI_NEUTRAL}{EMOJI_TIRED}{EMOJI_SHOCKED}");
const u8 gText_UnionRoomChatKeyboard_Emoji3[] = _("{EMOJI_LEAF}{EMOJI_FIRE}{EMOJI_WATER}{EMOJI_BOLT}{EMOJI_BALL}");
const u8 gText_UnionRoomChatKeyboard_Emoji4[] = _("♂♀{EMOJI_LEFT_PAREN}{EMOJI_RIGHT_PAREN}{EMOJI_TILDE}");
const u8 gText_UnionRoomChatKeyboard_Emoji5[] = _("{EMOJI_LEFT_EYE}{EMOJI_RIGHT_EYE}{EMOJI_SMALLWHEEL}{EMOJI_SPHERE}{EMOJI_IRRITATED}");
const u8 gText_UnionRoomChatKeyboard_Emoji6[] = _("{EMOJI_AT}{EMOJI_BIGWHEEL}{EMOJI_TONGUE}{EMOJI_ACUTE}{EMOJI_GRAVE}");
const u8 gText_UnionRoomChatKeyboard_Emoji7[] = _("{EMOJI_RIGHT_FIST}{EMOJI_LEFT_FIST}{EMOJI_TRIANGLE_OUTLINE}{EMOJI_UNION}{EMOJI_GREATER_THAN}");
const u8 gText_UnionRoomChatKeyboard_Emoji8[] = _("{EMOJI_CIRCLE}{EMOJI_TRIANGLE}{EMOJI_SQUARE}{EMOJI_HEART}{EMOJI_MOON}");
const u8 gText_UnionRoomChatKeyboard_Emoji9[] = _("{EMOJI_NOTE}{EMOJI_PLUS}{EMOJI_MINUS}{EMOJI_EQUALS}{EMOJI_PIPE}");
const u8 gText_UnionRoomChatKeyboard_Emoji10[] = _("{EMOJI_HIGHBAR}{EMOJI_UNDERSCORE};: ");
