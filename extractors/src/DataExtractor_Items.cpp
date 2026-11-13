// Layouts of the items group, see Layout.hpp and docs/RCT3_OVL_FORMAT.md.
//
//   cid CarriedItem, ced CarriedItemExtra, rcg ResearchCategory, psi ParticleSkin,
//   wai WildAnimalItem, was WildAnimalSpecies, ent Entertainer (EntertainerDesc).
//
// Sources: libOVLng carrieditems.h / wildanimalitem.h / ManagerCID.cpp, rerct3
// research (inventions.md, particles.md, animals.md), and the consumers in the
// Mac build (decompile/ghidra/decompiled.c), quoted per field below.
#include "ovl/extract/Layout.hpp"

#include <cstdio>

namespace ovl::layout {

namespace {

// Value of a symbol-reference slot, as K::Ref shows it; touches the slot.
Json ref_at(Ctx& c, std::uint32_t slot, const std::string& where) {
    const PairView& v = c.view();
    c.touch(slot);
    if (auto s = v.symref(slot)) return Json(*s);
    if (v.is_reloc(slot)) {
        const std::uint32_t t = v.u32(slot);
        for (auto& o : v.objects())
            if (o.data == t) return Json(o.name);
        return Json("<own data>");
    }
    if (std::uint32_t x = v.u32(slot)) {
        char b[48];
        std::snprintf(b, sizeof b, "non-relocated reference 0x%08X", x);
        c.issue(where, b);
    }
    return Json();
}

// `n` symbol-reference slots stored in place -> array of "name:tag".
Field inline_refs(std::uint32_t off, std::string name, std::uint32_t n) {
    return custom(off, std::move(name), 4 * n, [n](Ctx& c, std::uint32_t fa, std::uint32_t, const Json&) -> Json {
        Json out = Json::array();
        for (std::uint32_t i = 0; i < n; ++i) out.push(ref_at(c, fa + 4 * i, c.path + "[" + std::to_string(i) + "]"));
        return out;
    });
}

// Bit count of the EntertainerDesc animation masks, set by the parent before
// the mask entries are decoded (the mask length lives in the parent struct).
std::int64_t g_ent_anim_bits = 0;

}  // namespace

void register_items(Registry& r) {
    // ---- ced CarriedItemExtra (28 B) ------------------------------------------
    // libOVLng carrieditems.h struct CarriedItemExtra (condiments: ketchup, salt...).
    const Struct& ced = r.make("CarriedItemExtra", 0x1C, {
        zero(0x00),
        f(0x04, "name", K::Ref),        // txt
        f(0x08, "icon", K::Ref),        // gsi 20x20
        f(0x0C, "unk_0C"),              // 0, 4 for Mustard only
        f(0x10, "hunger", K::F32),
        f(0x14, "thirst", K::F32),
        f(0x18, "unk_18", K::F32),      // 0, 0.3 for Parmesan/Chilli/ChilliSauce/Marshmallow
    });
    r.add({"ced", "CarriedItemExtra", "", [&ced](const PairView&, std::uint32_t) { return &ced; }});

    // ---- cid CarriedItem (156 B) ----------------------------------------------
    // libOVLng carrieditems.h struct CarriedItem; the extras array (extra_count
    // ced references) is written right after each struct.
    const Struct& cid = r.make("CarriedItem", 0x9C, {
        f(0x00, "unk_00"),              // 1 in every object
        f(0x04, "addon_pack"),          // 0 vanilla, 1 Soaked!, 2 Wild!
        f(0x08, "unk_08"),              // 0, 2 for the Donut / Fries families
        f(0x0C, "name", K::Ref),        // txt
        f(0x10, "plural_name", K::Ref), // txt
        f(0x14, "icon", K::Ref),        // gsi 40x40, ":gsi" = none
        f(0x18, "shape", K::Ref),       // shs, null for hidden / billboard items
        f(0x1C, "shape_alt", K::Ref),   // shs, always equal to shape
        f(0x20, "lod_distance", K::F32),// 0 hidden, 40 shape items, 200 billboards
        f(0x24, "used_version", K::Ref),// cid: empty wrapper / open umbrella, ":cid" = none
        f(0x28, "flags", K::Hex32),     // 1 food, 2 drink, 4 used, 8 map, 0x10 voucher, 0x20 open umbrella, ...
        f(0x2C, "age_class", K::I32),   // -1, Soaked wearables 3 kids / 12 teens / 48 adults
        f(0x30, "carry_type"),          // 1 not visible, 2 hand, 4 body, 6 two hands, 8 head, 16 sunglasses, 36 swimsuit
        f(0x34, "hunger", K::F32),
        f(0x38, "thirst", K::F32),
        f(0x3C, "light_protection_factor", K::F32), // sun blockers 4 / 15 / 50
        f(0x40, "cost"),                // stall purchase cost, 1/100 currency
        f(0x44, "value"),               // price guests accept in normal weather
        f(0x48, "value_wet"),           // umbrella 450 vs 250, coffee > lemonade
        f(0x4C, "value_hot"),           // ice cream / lemonade > coffee
        f(0x50, "litter_rate", K::F32), // 0, 0.01 for litter (empty cups, wrappers...)
        f(0x54, "litter_kind", K::I32), // -1, 1 wrapper / stick, 2 box / bottle
        f(0x58, "default_colour"),      // flexi colour index: 0, 7 for Balloon
        f(0x5C, "extra_count"),
        arr(0x60, "extras", K::RefArray, Count::of("extra_count")),  // ced
        zero(0x64), zero(0x68), zero(0x6C),
        f(0x70, "shaped"),              // 1, 0 for billboard items
        f(0x74, "billboard_texture", K::Ref),  // ftx of a billboard item
        f(0x78, "billboard_scale_x", K::F32),
        f(0x7C, "billboard_scale_y", K::F32),
        f(0x80, "unk_80"),              // 0, 16 inflatables + swimsuits, 32 sunglasses
        f(0x84, "male_morph_body", K::Str),   // Soaked! wearables, "" otherwise
        f(0x88, "male_morph_legs", K::Str),
        f(0x8C, "female_morph_body", K::Str),
        f(0x90, "female_morph_legs", K::Str),
        f(0x94, "unk_94"),              // 0, 1 for SwimSuitV1
        f(0x98, "texture_option", K::Str),    // "Inflatable", "DefaultFlexiColours", "SwimSuitV1/2", ""
    });
    r.add({"cid", "CarriedItem", "", [&cid](const PairView&, std::uint32_t) { return &cid; }});

    // ---- rcg ResearchCategory (40 B) ------------------------------------------
    // ResearchCategoryDesc; consumer ResearchCategoryInstance (Mac FUN_10053c522
    // copies +0x0C/+0x10 to CurrentLevel/DevelopCost, IsHidden = (+0 == 2);
    // FUN_10053c61c hands +0x14/+0x18 to FUN_1005729ce and writes the instance
    // back-pointer to +0x1C; FUN_100572a52 tests each dependency's CurrentLevel
    // against its +8 float).
    const Struct& rcg_dep = r.make("ResearchDependency", 12, {
        f(0x00, "category", K::Ref),    // rcg
        f(0x04, "runtime_handle", K::I32), // -1 on disk; FUN_1005729ce writes FUN_100890de6()
        f(0x08, "required_level", K::F32), // percent of the category researched
    });
    const Struct& rcg = r.make("ResearchCategory", 0x28, {
        f(0x00, "initial_state"),       // 2 = hidden (IsHidden), 0 / 1 visible
        f(0x04, "name", K::Str),        // system name, duplicate check
        f(0x08, "display_name", K::Ref),// txt
        f(0x0C, "initial_level", K::F32), // CurrentLevel seed, percent
        f(0x10, "develop_cost", K::F32),  // DevelopCost
        f(0x14, "dependency_count"),
        sub(0x18, "dependencies", K::Array, rcg_dep, Count::of("dependency_count")),
        zero(0x1C),                     // runtime: ResearchCategoryInstance*
        zero(0x20), zero(0x24),
    });
    r.add({"rcg", "ResearchCategory", "", [&rcg](const PairView&, std::uint32_t) { return &rcg; }});

    // ---- psi ParticleSkin (28 B) ----------------------------------------------
    // rerct3 particles.md + ovl-extractor SFStructs.h ParticleSkin / SpriteCoords.
    const Struct& sprite = r.make("SpriteCoords", 16, {
        f(0x00, "left", K::F32), f(0x04, "top", K::F32), f(0x08, "right", K::F32), f(0x0C, "bottom", K::F32),
    });
    const Struct& psi = r.make("ParticleSkin", 0x1C, {
        f(0x00, "sprite_count"),
        sub(0x04, "sprites", K::PtrArray, sprite, Count::of("sprite_count")),
        f(0x08, "texture", K::Ref),     // tex
        f(0x0C, "type"),                // 0 normal, 1 special colour handling
        f(0x10, "mode"),                // 0 animated, 1 static
        f(0x14, "animation_speed", K::F32),
        zero(0x18),
    });
    r.add({"psi", "ParticleSkin", "", [&psi](const PairView&, std::uint32_t) { return &psi; }});

    // ---- wai WildAnimalItem (92 B) --------------------------------------------
    // libOVLng wildanimalitem.h.
    const Struct& wai = r.make("WildAnimalItem", 0x5C, {
        zero(0x00),
        f(0x04, "unk_04"), f(0x08, "unk_08"), f(0x0C, "unk_0C"),   // 2 in every object
        f(0x10, "short_name", K::Str),
        f(0x14, "shape", K::Ref),       // shs
        f(0x18, "lod_distance", K::F32),// 200
        f(0x1C, "cost", K::I32),
        f(0x20, "refund", K::I32),
        f(0x24, "wear_rate", K::F32),   // reciprocal durability: 0.2 food ... 0.033 rock
        f(0x28, "unk_28"),              // 0 ball / rock, 1 box / dummy, 2 iced meat / mango / tyre
        f(0x2C, "size_x", K::F32), f(0x30, "size_y", K::F32), f(0x34, "size_z", K::F32),
        f(0x38, "icon", K::Ref),        // gsi
        f(0x3C, "name", K::Ref),        // txt
        f(0x40, "unk_40", K::F32), f(0x44, "unk_44", K::F32), f(0x48, "unk_48", K::F32), f(0x4C, "unk_4C", K::F32),
        f(0x50, "animal_flags", K::Hex32), // 1 plant eaters, 2 meat eaters, 4 big animals
        f(0x54, "description", K::Ref), // txt
        f(0x58, "weight", K::F32),
    });
    r.add({"wai", "WildAnimalItem", "", [&wai](const PairView&, std::uint32_t) { return &wai; }});

    // ---- was WildAnimalSpecies (64 B head + 4 variants + 2 stat blocks) -------
    // Head labels: OVLExtractor-2 Form1.h "was"; stats: rerct3 animals.md
    // (WildAnimalSpeciesStats_FormatStatValueByIndex); genetics: Mac
    // FUN_100aa23bc / FUN_100aa24fc (WildAnimalGeneticStat_* by index);
    // variant sizes: Mac FUN_1002fb8b2 (+0x08..+0x10 around the animal);
    // sounds: Mac FUN_1002fcc72 / FUN_1002fc092 ("AnimalSound").
    const Struct& was_sound = r.make("AnimalSound", 0x2C, {
        f(0x00, "sound", K::Ref),       // snd
        f(0x04, "event"),               // animation / action id the sound is picked for
        f(0x08, "start_time", K::F32),  // > 0: stored as the one-shot's start time, else plays at once
        f(0x0C, "pitch_min", K::F32), f(0x10, "pitch_max", K::F32),     // random pitch factor
        f(0x14, "volume_min", K::F32), f(0x18, "volume_max", K::F32),   // random volume
        f(0x1C, "distance_min", K::F32), f(0x20, "distance_max", K::F32), // < 0: engine default
        f(0x24, "unk_24"),              // 0; passed to the sound-instance constructor
        f(0x28, "looping"),             // 1: looping sound object, 0: one-shot
    });
    const Struct& was_variant = r.make("WildAnimalVariant", 0x58, {
        f(0x00, "model", K::Ref),       // mdl
        f(0x04, "animations", K::Ref),  // wad
        f(0x08, "size_x", K::F32), f(0x0C, "size_y", K::F32), f(0x10, "size_z", K::F32),
        f(0x14, "unk_14"),              // 33000..60000, cubs larger than adults
        f(0x18, "unk_18", K::F32),
        f(0x1C, "description", K::Ref), // txt
        f(0x20, "unk_20", K::F32), f(0x24, "unk_24", K::F32),  // adults ~0.97/0.99, cubs ~0.5/0.45
        f(0x28, "unk_28"), f(0x2C, "unk_2C"),                   // adults 1/1, cubs 0/5
        f(0x30, "icon", K::Ref),        // gsi
        f(0x34, "unk_34", K::F32), f(0x38, "unk_38", K::F32),  // equal pairs, adults > cubs
        f(0x3C, "unk_3C", K::F32), f(0x40, "unk_40", K::F32), f(0x44, "unk_44", K::F32), f(0x48, "unk_48", K::F32),
        f(0x4C, "unk_4C"),              // 0..3
        f(0x50, "sound_count"),
        sub(0x54, "sounds", K::Array, was_sound, Count::of("sound_count")),
    });
    const Struct& was_genetics = r.make("WildAnimalGenetics", 0x18, {
        f(0x00, "hunger"), f(0x04, "peep_enjoyment"), f(0x08, "social"),
        f(0x0C, "water_body_requirement"), f(0x10, "adolescence_period"), f(0x14, "active"),
    });
    const Struct& was_stats = r.make("WildAnimalSpeciesStats", 0x24, {
        f(0x00, "prey_count"),
        arr(0x04, "prey", K::StrArray, Count::of("prey_count")),  // species names
        f(0x08, "breeding_happiness_req"),
        f(0x0C, "breeding_space_req"),
        f(0x10, "vertical_step_reach", K::F32),
        f(0x14, "diet_type"),           // 0 meat, 1 hay, 2 foliage
        f(0x18, "months_between_births"),
        f(0x1C, "babies_pre_pregnancy_av"),
        f(0x20, "gestation_period"),
    });
    const Struct& was = r.make("WildAnimalSpecies", 0x40, {
        zero(0x00),
        f(0x04, "name", K::Str),        // system name ("lion")
        f(0x08, "display_name", K::Ref),// txt
        f(0x0C, "description", K::Ref), // txt
        f(0x10, "icon", K::Ref),        // gsi
        f(0x14, "data_path", K::Str),   // "WildAnimals\lion\lion_data"
        sub(0x18, "adult_male", K::Ptr, was_variant),
        sub(0x1C, "adult_female", K::Ptr, was_variant),
        sub(0x20, "baby_male", K::Ptr, was_variant),
        sub(0x24, "baby_female", K::Ptr, was_variant),
        sub(0x28, "genetics", K::Ptr, was_genetics),
        sub(0x2C, "stats", K::Ptr, was_stats),
        f(0x30, "shelter", K::Str),     // AnimalShelterDesc name ("bigcatshelter"), FUN_10047c3a0
        f(0x34, "primate"),             // 1 only for chimp / gorilla / mandrill / orangutan; FUN_1004a963e
        f(0x38, "escape_height", K::F32), // FUN_1005532ae: compared with the enclosure's +0x50
        f(0x3C, "naming_name", K::Ref), // txt Name_WildAnimalSpecies_*_ForAnimalNaming
    });
    r.add({"was", "WildAnimalSpecies", "", [&was](const PairView&, std::uint32_t) { return &was; }});

    // ---- ent Entertainer = EntertainerDesc (256 B) ----------------------------
    // Characters/Entertainers/Entertainer%d and Characters/VIPStats/VIP%d; Mac
    // consumers: FUN_1002d4398 / FUN_100a37e60 (parts +0x0C/+0x10),
    // FUN_1002d5fbc / FUN_100a693b6 (speed factors +0x04/+0x08),
    // FUN_1005df742 (debug name +0x14), FUN_1003be553 (height +0x18),
    // FUN_1006d035c (thoughts +0xA4/+0xA8), FUN_1002d5c2c (animation masks
    // +0xAC..+0xB4), FUN_100994be0 (+0xB8/+0xBC), FUN_1009949ac (+0xC0..+0xFC).
    const Struct& ent_thought = r.make("EntertainerThought", 12, {
        f(0x00, "thought"),             // thought id the guest code asks for
        f(0x04, "text_count"),
        arr(0x08, "texts", K::RefArray, Count::of("text_count")),  // txt, picked at random
    });
    const Struct& ent_mask = r.make("EntertainerAnimMask", 16, {
        f(0x00, "flag_word"),           // which of the guest's animation flag words (0..2) gets the bits
        f(0x04, "flags_if_set", K::Hex32),
        f(0x08, "flags_out_of_range", K::Hex32), // used when the animation id >= anim_count
        custom(0x0C, "mask", 4, [](Ctx& c, std::uint32_t fa, std::uint32_t, const Json&) -> Json {
            auto p = c.pointer(fa, c.path + "+0xC");
            if (!p) return Json();
            const std::uint32_t words = std::uint32_t((g_ent_anim_bits + 31) / 32);
            c.cover(*p, 4 * words);
            Json out = Json::array();
            for (std::uint32_t i = 0; i < words; ++i) {
                char b[16];
                std::snprintf(b, sizeof b, "%08X", c.view().u32(*p + 4 * i));
                out.push(Json(std::string(b)));
            }
            return out;
        }),
    });
    const Struct& ent_offset = r.make("PoseOffset", 8, {
        f(0x00, "z", K::F32), f(0x04, "y", K::F32),
    });
    const Struct& ent = r.make("EntertainerDesc", 0x100, {
        zero(0x00),
        f(0x04, "speed_factor_count"),
        arr(0x08, "speed_factors", K::Scalars, Count::of("speed_factor_count"), K::F32),
        f(0x0C, "part_count"),
        arr(0x10, "parts", K::StrArray, Count::of("part_count")),  // people parts: head, body, legs, swim body, swim legs
        f(0x14, "debug_name", K::Str),
        f(0x18, "height", K::F32),
        f(0x1C, "display_name", K::Ref),// txt
        f(0x20, "costume_icon", K::Ref),// gsi, ":gsi" for VIPs
        inline_refs(0x24, "lower_colours", 16),  // gsi <name>Lower_UniformColour_N
        inline_refs(0x64, "upper_colours", 16),  // gsi <name>Upper_UniformColour_N
        f(0xA4, "thought_count"),
        sub(0xA8, "thoughts", K::Array, ent_thought, Count::of("thought_count")),
        f(0xAC, "anim_count"),          // bits per animation mask (329 / 348)
        f(0xB0, "anim_mask_count"),
        custom(0xB4, "anim_masks", 4, [&ent_mask](Ctx& c, std::uint32_t fa, std::uint32_t base, const Json&) -> Json {
            const std::int64_t n = c.view().u32(base + 0xB0);
            g_ent_anim_bits = c.view().u32(base + 0xAC);
            auto p = c.pointer(fa, c.path + "+0xB4");
            if (!p) {
                if (n) c.issue(c.path + "+0xB4", "null pointer with count " + std::to_string(n));
                return Json::array();
            }
            Json out = Json::array();
            for (std::int64_t i = 0; i < n; ++i) out.push(c.decode(ent_mask, *p + std::uint32_t(i) * ent_mask.size));
            return out;
        }),
        f(0xB8, "pose_time_0", K::I32), // FUN_100994be0(0, ...), divided by a constant
        f(0xBC, "pose_time_1", K::I32), // FUN_100994be0(1, ...)
        sub(0xC0, "pose_offsets", K::Inline, ent_offset, Count::n(8)),  // poses 2..9, x = 0
    });
    r.add({"ent", "Entertainer", "", [&ent](const PairView&, std::uint32_t) { return &ent; }});
}

}  // namespace ovl::layout
