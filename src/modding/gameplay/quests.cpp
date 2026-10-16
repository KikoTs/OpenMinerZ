// Quests and dialogue: a small story library on top of the events, stats and
// entities. Progress is server-side, persisted per player (by gamer tag) in
// the sidecar and replicated to the owning client as its quest log; dialogue
// is a server-side session whose current node the client's dialogue screen
// draws (ClientDialogue) and answers with a choice index.

#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

[[nodiscard]] bool truthy(const Value &value)
{
    return !value.is_nil() && !(value.type() == ValueType::boolean && !value.as_bool());
}

[[nodiscard]] ObjectiveKind objective_kind(std::string_view name)
{
    if (name == "kill")
        return ObjectiveKind::kill;
    if (name == "collect")
        return ObjectiveKind::collect;
    if (name == "reach")
        return ObjectiveKind::reach;
    if (name == "talk")
        return ObjectiveKind::talk;
    if (name == "custom")
        return ObjectiveKind::custom;
    throw ScriptError{std::format("unknown objective type '{}' (kill, collect, reach, talk, custom)", name)};
}

[[nodiscard]] std::string default_objective_text(const ObjectiveDef &objective)
{
    switch (objective.kind)
    {
    case ObjectiveKind::kill:
        return std::format("Defeat {} {}", objective.count, objective.target);
    case ObjectiveKind::collect:
        return std::format("Collect {} {}", objective.count, objective.target);
    case ObjectiveKind::reach:
        return "Reach the place";
    case ObjectiveKind::talk:
        return std::format("Talk to {}", objective.target);
    case ObjectiveKind::custom:
        break;
    }
    return objective.id;
}

} // namespace

// ------------------------------------------------------------ definitions

void GameplayService::Impl::define_quest(Side side, std::string_view mod, std::string_view id,
                                         const Value &def)
{
    Registry &definitions = registry(side);
    if (id.empty() || id.size() > 64U)
        throw ScriptError{"a quest id is 1 to 64 characters"};
    const auto existing = definitions.quests.find(id);
    if (existing != definitions.quests.end() && existing->second.mod != mod)
        throw ScriptError{std::format("quest '{}' is already defined by '{}'", id, existing->second.mod)};
    QuestDef quest;
    quest.id = std::string{id};
    quest.mod = std::string{mod};
    quest.title = string_or(def, "title", id);
    quest.description = string_or(def, "description", "");
    quest.repeatable = bool_or(def, "repeatable", false);
    const Value *stages = def.field("stages");
    if (stages == nullptr || stages->type() != ValueType::array || stages->as_array().empty())
        throw ScriptError{"a quest needs stages = { {text = ..., objectives = {...}}, ... }"};
    for (const Value &stage_value : stages->as_array())
    {
        if (quest.stages.size() >= 32U)
            throw ScriptError{"a quest has at most 32 stages"};
        StageDef stage;
        stage.id = string_or(stage_value, "id", std::format("stage{}", quest.stages.size() + 1U));
        stage.text = string_or(stage_value, "text", "");
        if (const Value *objectives = stage_value.field("objectives");
            objectives != nullptr && objectives->type() == ValueType::array)
            for (const Value &objective_value : objectives->as_array())
            {
                if (stage.objectives.size() >= 16U)
                    throw ScriptError{"a stage has at most 16 objectives"};
                ObjectiveDef objective;
                objective.kind = objective_kind(string_or(objective_value, "type", "custom"));
                objective.id = string_or(objective_value, "id",
                                         std::format("objective{}", stage.objectives.size() + 1U));
                objective.target = string_or(objective_value, "target", "");
                objective.count = static_cast<std::uint32_t>(std::clamp(number_or(objective_value, "count", 1.0), 1.0, 1'000'000.0));
                if (objective.kind == ObjectiveKind::reach)
                {
                    const Value *position = objective_value.field("position");
                    if (position == nullptr)
                        throw ScriptError{"a reach objective needs a position"};
                    objective.position = to_vec3(*position, "position");
                    objective.radius = static_cast<float>(std::clamp(number_or(objective_value, "radius", 4.0), 0.5, 1000.0));
                    objective.count = 1U;
                }
                if (objective.kind == ObjectiveKind::collect)
                    static_cast<void>(resolve_item(Value{objective.target}, mod, "collect target", side));
                objective.text = string_or(objective_value, "text", default_objective_text(objective));
                stage.objectives.push_back(std::move(objective));
            }
        quest.stages.push_back(std::move(stage));
    }
    if (const Value *rewards = def.field("rewards"); rewards != nullptr && rewards->type() == ValueType::map)
        if (const Value *items = rewards->field("items"); items != nullptr && items->type() == ValueType::array)
            for (const Value &entry : items->as_array())
            {
                if (entry.type() == ValueType::array && entry.as_array().size() == 2U)
                    quest.reward_items.emplace_back(resolve_item(entry.as_array()[0], mod, "reward", side),
                                                    static_cast<std::uint16_t>(std::clamp(entry.as_array()[1].as_number(), 1.0, 999.0)));
                else
                    quest.reward_items.emplace_back(
                        resolve_item(entry.field_or("item", Value{}), mod, "reward", side),
                        static_cast<std::uint16_t>(std::clamp(number_or(entry, "count", 1.0), 1.0, 999.0)));
            }
    std::vector<Callback> kept;
    if (side == Side::server)
    {
        quest.on_stage = callback_or_none(def, "on_stage");
        quest.on_complete = callback_or_none(def, "on_complete");
        for (const Callback &callback : {quest.on_stage, quest.on_complete})
            if (callback.valid())
            {
                kept.push_back(callback);
                hold(side, callback);
            }
    }
    release_unused(def, kept);
    definitions.quests.insert_or_assign(quest.id, std::move(quest));
}

void GameplayService::Impl::define_dialogue(Side side, std::string_view mod, std::string_view id,
                                            const Value &def)
{
    Registry &definitions = registry(side);
    if (id.empty() || id.size() > 64U)
        throw ScriptError{"a dialogue id is 1 to 64 characters"};
    const auto existing = definitions.dialogues.find(id);
    if (existing != definitions.dialogues.end() && existing->second.mod != mod)
        throw ScriptError{std::format("dialogue '{}' is already defined by '{}'", id, existing->second.mod)};
    DialogueDef dialogue;
    dialogue.id = std::string{id};
    dialogue.mod = std::string{mod};
    dialogue.title = string_or(def, "title", id);
    const Value *nodes = def.field("nodes");
    if (nodes == nullptr || nodes->type() != ValueType::map || nodes->as_map().empty())
        throw ScriptError{"a dialogue needs nodes = { name = {text = ..., choices = {...}} }"};
    std::vector<Callback> kept;
    const auto keep = [&](const Callback &callback) {
        if (callback.valid() && side == Side::server)
        {
            kept.push_back(callback);
            hold(side, callback);
        }
    };
    for (const auto &[name, node_value] : nodes->as_map())
    {
        if (dialogue.nodes.size() >= 256U)
            throw ScriptError{"a dialogue has at most 256 nodes"};
        DialogueNode node;
        node.id = name;
        node.speaker = string_or(node_value, "speaker", dialogue.title);
        node.text = string_or(node_value, "text", "");
        node.portrait = string_or(node_value, "portrait", "");
        node.next = string_or(node_value, "next", "");
        node.on_enter = callback_or_none(node_value, "on_enter");
        keep(node.on_enter);
        if (const Value *choices = node_value.field("choices"); choices != nullptr && choices->type() == ValueType::array)
            for (const Value &choice_value : choices->as_array())
            {
                if (node.choices.size() >= 16U)
                    throw ScriptError{"a dialogue node has at most 16 choices"};
                DialogueChoice choice;
                choice.text = string_or(choice_value, "text", "...");
                choice.next = string_or(choice_value, "next", "");
                choice.start_quest = string_or(choice_value, "start_quest", "");
                choice.condition = callback_or_none(choice_value, "condition");
                choice.action = callback_or_none(choice_value, "action");
                keep(choice.condition);
                keep(choice.action);
                node.choices.push_back(std::move(choice));
            }
        dialogue.nodes.emplace(name, std::move(node));
    }
    dialogue.start = string_or(def, "start", dialogue.nodes.begin()->first);
    if (!dialogue.nodes.contains(dialogue.start))
        throw ScriptError{std::format("dialogue '{}': no start node '{}'", id, dialogue.start)};
    for (const auto &[name, node] : dialogue.nodes)
    {
        if (!node.next.empty() && !dialogue.nodes.contains(node.next))
            throw ScriptError{std::format("dialogue '{}': node '{}' continues to unknown '{}'", id, name, node.next)};
        for (const DialogueChoice &choice : node.choices)
            if (!choice.next.empty() && !dialogue.nodes.contains(choice.next))
                throw ScriptError{std::format("dialogue '{}': a choice of '{}' leads to unknown '{}'", id, name, choice.next)};
    }
    release_unused(def, kept);
    definitions.dialogues.insert_or_assign(dialogue.id, std::move(dialogue));
}

// ----------------------------------------------------------------- quests

bool GameplayService::Impl::start_quest(PlayerRecord &player, std::string_view id)
{
    const auto quest = server_registry.quests.find(id);
    if (quest == server_registry.quests.end())
        throw ScriptError{std::format("no quest '{}'", id)};
    const auto existing = player.quests.find(id);
    if (existing != player.quests.end() && !existing->second.completed)
        return false;
    if (player.completed_quests.contains(id) && !quest->second.repeatable)
        return false;
    QuestProgress progress;
    progress.objectives.resize(quest->second.stages.front().objectives.size());
    player.quests.insert_or_assign(std::string{id}, std::move(progress));
    player.quests_dirty = true;
    emit(Side::server, "quest.started", make_map({{"player", handle_of(player)}, {"quest", std::string{id}}}));
    quest_event(player, ObjectiveKind::collect, {}, 0U);
    return true;
}

void GameplayService::Impl::complete_quest(PlayerRecord &player, std::string_view id)
{
    const auto quest = server_registry.quests.find(id);
    auto progress = player.quests.find(id);
    if (quest == server_registry.quests.end() || progress == player.quests.end() ||
        progress->second.completed)
        return;
    progress->second.completed = true;
    player.completed_quests.insert(std::string{id});
    player.quests_dirty = true;
    for (const auto &[item, amount] : quest->second.reward_items)
        static_cast<void>(give(player, item, amount));
    if (quest->second.on_complete.valid())
    {
        const std::array<Value, 2> call{Value{handle_of(player)}, Value{std::string{id}}};
        static_cast<void>(host.invoke_as(handle_of(player), quest->second.on_complete, call));
    }
    emit(Side::server, "quest.completed", make_map({{"player", handle_of(player)}, {"quest", std::string{id}}}));
}

bool GameplayService::Impl::abandon_quest(PlayerRecord &player, std::string_view id)
{
    const auto progress = player.quests.find(id);
    if (progress == player.quests.end() || progress->second.completed)
        return false;
    player.quests.erase(progress);
    player.quests_dirty = true;
    emit(Side::server, "quest.abandoned", make_map({{"player", handle_of(player)}, {"quest", std::string{id}}}));
    return true;
}

void GameplayService::Impl::advance_objective(PlayerRecord &player, std::string_view quest_id,
                                              std::string_view objective_id, std::uint32_t amount)
{
    const auto quest = server_registry.quests.find(quest_id);
    if (quest == server_registry.quests.end())
        throw ScriptError{std::format("no quest '{}'", quest_id)};
    const auto progress = player.quests.find(quest_id);
    if (progress == player.quests.end() || progress->second.completed)
        return;
    const StageDef &stage = quest->second.stages[progress->second.stage];
    for (std::size_t index{}; index < stage.objectives.size(); ++index)
    {
        if (!objective_id.empty() && stage.objectives[index].id != objective_id)
            continue;
        ObjectiveProgress &state = progress->second.objectives[index];
        if (state.done)
            continue;
        state.progress = std::min(stage.objectives[index].count, state.progress + amount);
        state.done = state.progress >= stage.objectives[index].count;
        player.quests_dirty = true;
        emit(Side::server, "quest.progress",
             make_map({{"player", handle_of(player)},
                       {"quest", std::string{quest_id}},
                       {"objective", stage.objectives[index].id},
                       {"progress", static_cast<std::int64_t>(state.progress)},
                       {"required", static_cast<std::int64_t>(stage.objectives[index].count)}}));
    }
    // A stage whose objectives are all done moves the quest on.
    auto current = player.quests.find(quest_id);
    if (current == player.quests.end() || current->second.completed)
        return;
    const bool stage_done = std::ranges::all_of(current->second.objectives,
                                                [](const ObjectiveProgress &state) { return state.done; });
    if (!stage_done)
        return;
    if (current->second.stage + 1U >= quest->second.stages.size())
    {
        complete_quest(player, quest_id);
        return;
    }
    ++current->second.stage;
    current->second.objectives.assign(quest->second.stages[current->second.stage].objectives.size(), {});
    player.quests_dirty = true;
    const std::string stage_id = quest->second.stages[current->second.stage].id;
    if (quest->second.on_stage.valid())
    {
        const std::array<Value, 3> call{Value{handle_of(player)}, Value{std::string{quest_id}}, Value{stage_id}};
        static_cast<void>(host.invoke_as(handle_of(player), quest->second.on_stage, call));
    }
    emit(Side::server, "quest.stage",
         make_map({{"player", handle_of(player)}, {"quest", std::string{quest_id}}, {"stage", stage_id}}));
    // The next stage may already be satisfied (a collect objective).
    quest_event(player, ObjectiveKind::collect, {}, 0U);
}

void GameplayService::Impl::quest_event(PlayerRecord &player, ObjectiveKind kind,
                                        std::string_view target, std::uint32_t amount)
{
    // Walk a copy of the ids: completing one may start another.
    std::vector<std::string> active;
    for (const auto &[id, progress] : player.quests)
        if (!progress.completed)
            active.push_back(id);
    for (const std::string &id : active)
    {
        const auto quest = server_registry.quests.find(id);
        const auto progress = player.quests.find(id);
        if (quest == server_registry.quests.end() || progress == player.quests.end() ||
            progress->second.completed)
            continue;
        const StageDef &stage = quest->second.stages[progress->second.stage];
        for (std::size_t index{}; index < stage.objectives.size(); ++index)
        {
            const ObjectiveDef &objective = stage.objectives[index];
            if (objective.kind != kind || progress->second.objectives[index].done)
                continue;
            if (kind == ObjectiveKind::collect)
            {
                // Collect is a level, not a count of events: how many are
                // carried now.
                std::uint32_t have{};
                try
                {
                    have = count(player, resolve_item(Value{objective.target}, quest->second.mod, "collect"));
                }
                catch (const ScriptError &)
                {
                    continue;
                }
                const std::uint32_t before = progress->second.objectives[index].progress;
                const std::uint32_t now = std::min(have, objective.count);
                if (now > before)
                    advance_objective(player, id, objective.id, now - before);
                else if (now < before)
                {
                    progress->second.objectives[index].progress = now;
                    player.quests_dirty = true;
                }
                break;
            }
            if (objective.target.empty() || objective.target == "any" || objective.target == target)
            {
                advance_objective(player, id, objective.id, amount);
                break;
            }
        }
    }
}

void GameplayService::Impl::tick_quests()
{
    for (PlayerRecord &player : players)
    {
        if (!player.present)
            continue;
        bool any_collect{};
        std::vector<std::pair<std::string, std::string>> reached;
        for (const auto &[id, progress] : player.quests)
        {
            if (progress.completed)
                continue;
            const auto quest = server_registry.quests.find(id);
            if (quest == server_registry.quests.end())
                continue;
            const StageDef &stage = quest->second.stages[progress.stage];
            for (std::size_t index{}; index < stage.objectives.size(); ++index)
            {
                const ObjectiveDef &objective = stage.objectives[index];
                if (progress.objectives[index].done)
                    continue;
                if (objective.kind == ObjectiveKind::collect)
                    any_collect = true;
                if (objective.kind == ObjectiveKind::reach &&
                    distance_squared(player.position, objective.position) <= objective.radius * objective.radius)
                    reached.emplace_back(id, objective.id);
            }
        }
        for (const auto &[quest, objective] : reached)
            advance_objective(player, quest, objective, 1U);
        if (any_collect && player.local)
            quest_event(player, ObjectiveKind::collect, {}, 0U);
    }
}

Value GameplayService::Impl::quest_status(const PlayerRecord &player, std::string_view id) const
{
    const auto quest = server_registry.quests.find(id);
    if (quest == server_registry.quests.end())
        throw ScriptError{std::format("no quest '{}'", id)};
    const auto progress = player.quests.find(id);
    if (progress == player.quests.end())
        return make_map({{"id", std::string{id}},
                         {"active", false},
                         {"completed", player.completed_quests.contains(id)}});
    const StageDef &stage = quest->second.stages[progress->second.stage];
    Array objectives;
    for (std::size_t index{}; index < stage.objectives.size(); ++index)
        objectives.push_back(make_map({{"id", stage.objectives[index].id},
                                       {"progress", static_cast<std::int64_t>(progress->second.objectives[index].progress)},
                                       {"required", static_cast<std::int64_t>(stage.objectives[index].count)},
                                       {"done", progress->second.objectives[index].done}}));
    return make_map({{"id", std::string{id}},
                     {"active", !progress->second.completed},
                     {"completed", progress->second.completed || player.completed_quests.contains(id)},
                     {"stage", stage.id},
                     {"stage_index", static_cast<std::int64_t>(progress->second.stage + 1U)},
                     {"objectives", Value{std::move(objectives)}}});
}

void GameplayService::Impl::replicate_quests(PlayerRecord &player)
{
    player.quests_dirty = false;
    Array list;
    for (const auto &[id, progress] : player.quests)
    {
        const auto quest = server_registry.quests.find(id);
        if (quest == server_registry.quests.end() || list.size() >= 64U)
            continue;
        const StageDef &stage = quest->second.stages[std::min(progress.stage, quest->second.stages.size() - 1U)];
        Array objectives;
        for (std::size_t index{}; index < stage.objectives.size() && index < progress.objectives.size(); ++index)
            objectives.push_back(make_map({{"text", stage.objectives[index].text},
                                           {"progress", static_cast<std::int64_t>(progress.objectives[index].progress)},
                                           {"required", static_cast<std::int64_t>(stage.objectives[index].count)},
                                           {"done", progress.objectives[index].done}}));
        list.push_back(make_map({{"id", id},
                                 {"title", quest->second.title},
                                 {"description", quest->second.description},
                                 {"stage", stage.text},
                                 {"completed", progress.completed},
                                 {"objectives", Value{std::move(objectives)}}}));
    }
    out().to_player(player.gid, make_map({{"t", "quests"}, {"list", Value{std::move(list)}}}), true);
}

// --------------------------------------------------------------- dialogue

bool GameplayService::Impl::open_dialogue(PlayerRecord &player, std::string_view id,
                                          std::string_view node, std::uint64_t entity_id)
{
    const auto dialogue = server_registry.dialogues.find(id);
    if (dialogue == server_registry.dialogues.end())
        throw ScriptError{std::format("no dialogue '{}'", id)};
    const std::string start = node.empty() ? dialogue->second.start : std::string{node};
    if (!dialogue->second.nodes.contains(start))
        throw ScriptError{std::format("dialogue '{}' has no node '{}'", id, start)};
    if (player.dialogue.has_value())
        close_dialogue(player, false);
    DialogueSession session;
    session.id = next_dialogue_id++;
    session.dialogue = std::string{id};
    session.node = start;
    session.entity = entity_id;
    player.dialogue = std::move(session);
    emit(Side::server, "dialogue.opened",
         make_map({{"player", handle_of(player)}, {"dialogue", std::string{id}}, {"node", start}}));
    if (player.dialogue.has_value())
        show_dialogue_node(player);
    return true;
}

void GameplayService::Impl::show_dialogue_node(PlayerRecord &player)
{
    if (!player.dialogue.has_value())
        return;
    const auto dialogue = server_registry.dialogues.find(player.dialogue->dialogue);
    if (dialogue == server_registry.dialogues.end())
    {
        close_dialogue(player, true);
        return;
    }
    const auto node = dialogue->second.nodes.find(player.dialogue->node);
    if (node == dialogue->second.nodes.end())
    {
        close_dialogue(player, true);
        return;
    }
    if (node->second.on_enter.valid())
    {
        const std::array<Value, 2> call{Value{handle_of(player)}, Value{node->first}};
        static_cast<void>(host.invoke_as(handle_of(player), node->second.on_enter, call));
        if (!player.dialogue.has_value() || player.dialogue->node != node->first)
            return; // the handler closed or redirected it
    }
    player.dialogue->shown.clear();
    Array choices;
    for (std::size_t index{}; index < node->second.choices.size(); ++index)
    {
        const DialogueChoice &choice = node->second.choices[index];
        if (choice.condition.valid())
        {
            const std::array<Value, 1> call{Value{handle_of(player)}};
            if (!truthy(host.invoke_as(handle_of(player), choice.condition, call)))
                continue;
        }
        player.dialogue->shown.push_back(index);
        choices.emplace_back(choice.text);
    }
    if (choices.empty())
        choices.emplace_back(node->second.next.empty() ? std::string{"Goodbye"} : std::string{"Continue"});
    out().to_player(player.gid,
                    make_map({{"t", "dialogue"},
                              {"session", static_cast<std::int64_t>(player.dialogue->id)},
                              {"title", dialogue->second.title},
                              {"speaker", node->second.speaker},
                              {"text", node->second.text},
                              {"portrait", node->second.portrait},
                              {"entity", static_cast<std::int64_t>(player.dialogue->entity)},
                              {"choices", Value{std::move(choices)}}}),
                    true);
}

void GameplayService::Impl::choose(PlayerRecord &player, std::uint64_t session, std::size_t choice)
{
    if (!player.dialogue.has_value() || player.dialogue->id != session)
        return;
    const auto dialogue = server_registry.dialogues.find(player.dialogue->dialogue);
    if (dialogue == server_registry.dialogues.end())
    {
        close_dialogue(player, true);
        return;
    }
    const auto node = dialogue->second.nodes.find(player.dialogue->node);
    if (node == dialogue->second.nodes.end())
    {
        close_dialogue(player, true);
        return;
    }
    const std::string dialogue_id = dialogue->first;
    const std::string node_id = node->first;
    if (player.dialogue->shown.empty())
    {
        // The single Continue/Goodbye.
        if (choice != 0U)
            return;
        const std::string next = node->second.next;
        emit(Side::server, "dialogue.choice",
             make_map({{"player", handle_of(player)}, {"dialogue", dialogue_id}, {"node", node_id},
                       {"choice", 1}, {"text", next.empty() ? "Goodbye" : "Continue"}}));
        if (!player.dialogue.has_value() || player.dialogue->id != session)
            return;
        if (next.empty())
            close_dialogue(player, true);
        else
        {
            player.dialogue->node = next;
            show_dialogue_node(player);
        }
        return;
    }
    if (choice >= player.dialogue->shown.size())
        return;
    const DialogueChoice picked = node->second.choices[player.dialogue->shown[choice]];
    emit(Side::server, "dialogue.choice",
         make_map({{"player", handle_of(player)}, {"dialogue", dialogue_id}, {"node", node_id},
                   {"choice", static_cast<std::int64_t>(choice + 1U)}, {"text", picked.text}}));
    if (!picked.start_quest.empty() && server_registry.quests.contains(picked.start_quest))
        static_cast<void>(start_quest(player, picked.start_quest));
    if (picked.action.valid())
    {
        const std::array<Value, 1> call{Value{handle_of(player)}};
        static_cast<void>(host.invoke_as(handle_of(player), picked.action, call));
    }
    // The action may have closed the dialogue or opened another.
    if (!player.dialogue.has_value() || player.dialogue->id != session)
        return;
    if (picked.next.empty())
        close_dialogue(player, true);
    else
    {
        player.dialogue->node = picked.next;
        show_dialogue_node(player);
    }
}

void GameplayService::Impl::close_dialogue(PlayerRecord &player, bool notify_client)
{
    if (!player.dialogue.has_value())
        return;
    const DialogueSession session = *player.dialogue;
    player.dialogue.reset();
    if (notify_client)
        out().to_player(player.gid,
                        make_map({{"t", "dialogue"}, {"session", static_cast<std::int64_t>(session.id)},
                                  {"close", true}}),
                        true);
    emit(Side::server, "dialogue.closed",
         make_map({{"player", handle_of(player)}, {"dialogue", session.dialogue}}));
}

} // namespace openminerz::modding::gameplay
