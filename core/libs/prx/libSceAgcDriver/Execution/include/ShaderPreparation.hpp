#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERPREPARATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERPREPARATION_HPP

#include <memory>

namespace AgcDriver::DriverDetail {

struct ShaderSnapshot;
struct PreparedShaderState;

class ShaderPreparationTransaction {
public:
    ShaderPreparationTransaction();
    ~ShaderPreparationTransaction();
    ShaderPreparationTransaction(const ShaderPreparationTransaction&) = delete;
    ShaderPreparationTransaction& operator=(const ShaderPreparationTransaction&) = delete;
    PreparedShaderState& Edit(const ShaderSnapshot& snapshot);
    const PreparedShaderState& Read(const ShaderSnapshot& snapshot) const;
    void Commit();
    // Gives up the writers' lock for a long preparation when the transaction (its outermost one)
    // has edited nothing yet: what it read is dropped, so its next Read or Edit sees what other
    // threads committed meanwhile, and references from earlier Reads must not be used after it.
    // Returns false (and keeps the lock) when an edit is pending. Resume takes the lock back.
    bool Suspend();
    void Resume();

private:
    struct State;
    std::unique_ptr<State> state;
    State* root = nullptr;
    bool committed = false;
};

}

#endif
