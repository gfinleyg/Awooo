#pragma once

// A workout as the app rides it: its blocks with their targets at every difficulty, and its timed messages.
// The Python app kept the blocks as tuples in AppState.workoutList; these are the same entries, typed.

#include <QList>
#include <QString>

#include <array>

namespace awo {

// Every target is worked out at each of 21 difficulties, -10 to +10 steps; index 10 is the workout as written
constexpr int kDifficulties = 21;
constexpr int kBaseDifficulty = 10;
using Targets = std::array<int, kDifficulties>;

enum class BlockType { FreeRide = 0, Steady = 1, Ramp = 2 };

// Riding positions, as an .awo block's last value numbers them
enum Position { Seated = 0, SeatedHighCadence = 1, SeatedLowCadence = 2, Standing = 3, SingleLeg = 4,
                SwitchSingleLeg = 5 };

// A free ride, steady or ramp block
struct Block {
    BlockType type = BlockType::FreeRide;
    int seconds = 0;
    int position = Seated;
    Targets power{};     // steady: the target; ramp: where it starts (watts, or RPM in Musical Cadence Mode)
    Targets powerEnd{};  // ramp: where it ends

    bool operator==(const Block&) const = default;
};

// One entry of the workout's list, which the workout screen walks in order:
//   Block           a block on its own
//   Interval        an interval's header (its blocks and repeats), followed by an IntervalBlock entry for each of
//                   its blocks in each repeat
//   IntervalBlock   one block of one repeat of the interval before it
//   RampStep        a ramp's next target and how long it's held; a ramp block (on its own or an interval's) is
//                   followed by its steps, which cover the whole ramp
struct WorkoutEntry {
    enum class Type { Block = 0, Interval = 3, RampStep = 4, IntervalBlock = 5 };

    Type type = Type::Block;
    Block block;                  // Block, IntervalBlock: the block
    QList<Block> intervalBlocks;  // Interval: its 2 or 3 blocks
    int repeats = 0;              // Interval: how many; IntervalBlock: the repeats left, this one included
    int subIndex = 0;             // IntervalBlock: which of the interval's blocks
    int stepSeconds = 0;          // RampStep: how long the step's target is held
    Targets stepPower{};          // RampStep: its target
    int rampSeconds = 0;          // RampStep: the whole ramp's length

    static WorkoutEntry ofBlock(const Block& block) {
        WorkoutEntry entry;
        entry.block = block;
        return entry;
    }
    static WorkoutEntry interval(int repeats, const QList<Block>& blocks) {
        WorkoutEntry entry;
        entry.type = Type::Interval;
        entry.repeats = repeats;
        entry.intervalBlocks = blocks;
        return entry;
    }
    static WorkoutEntry intervalBlock(int repeatsLeft, int subIndex, const Block& block) {
        WorkoutEntry entry;
        entry.type = Type::IntervalBlock;
        entry.repeats = repeatsLeft;
        entry.subIndex = subIndex;
        entry.block = block;
        return entry;
    }
    static WorkoutEntry rampStep(int seconds, const Targets& power, int rampSeconds) {
        WorkoutEntry entry;
        entry.type = Type::RampStep;
        entry.stepSeconds = seconds;
        entry.stepPower = power;
        entry.rampSeconds = rampSeconds;
        return entry;
    }

    bool operator==(const WorkoutEntry&) const = default;
};

// A message shown from start to end, in seconds of workout time
struct Message {
    int start = 0;
    int end = 0;
    QString text;

    bool operator==(const Message&) const = default;
};

struct Workout {
    QString title;  // from its T: line, empty without one
    int seconds = 0;
    QList<WorkoutEntry> entries;
    QList<Message> messages;
};

}  // namespace awo
