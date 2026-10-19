package com.sbro.emucorer.data

object GamepadBindingRules {

    fun assign(bindings: Map<String, Int>, actionId: String, keyCode: Int): Map<String, Int> {
        val updated = bindings.toMutableMap()
        updated.entries.removeAll { (existingAction, existingKey) ->
            existingKey == keyCode && existingAction != actionId
        }
        updated[actionId] = keyCode
        return updated
    }

    fun merge(
        base: Map<Int, Map<String, Int>>,
        overrides: Map<Int, Map<String, Int>>
    ): Map<Int, Map<String, Int>> {
        val result = base.toMutableMap()
        overrides.forEach { (padIndex, padOverrides) ->
            var bindings = result[padIndex].orEmpty()
            padOverrides.forEach { (actionId, keyCode) ->
                bindings = assign(bindings, actionId, keyCode)
            }
            result[padIndex] = bindings
        }
        return result
    }
}
