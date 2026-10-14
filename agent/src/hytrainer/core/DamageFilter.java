// =============================================================================
//  DamageFilter.java — ECS damage system in the game's filter group.
//
//  Runs for every damage event before ApplyDamage subtracts health:
//    - damage to the owner: cancelled in god mode, fall damage cancelled with No Fall
//    - damage dealt by the owner: one-hit kill or multiplier
//
//  Registered and unregistered from the agent's own thread only: the registry
//  update takes the store data lock that world threads hold while ticking.
// =============================================================================
package hytrainer.core;

import com.hypixel.hytale.component.ArchetypeChunk;
import com.hypixel.hytale.component.CommandBuffer;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.component.SystemGroup;
import com.hypixel.hytale.component.query.Query;
import com.hypixel.hytale.server.core.modules.entity.damage.Damage;
import com.hypixel.hytale.server.core.modules.entity.damage.DamageCause;
import com.hypixel.hytale.server.core.modules.entity.damage.DamageEventSystem;
import com.hypixel.hytale.server.core.modules.entity.damage.DamageModule;
import com.hypixel.hytale.server.core.modules.entitystats.EntityStatMap;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;

final class DamageFilter extends DamageEventSystem {
    private static volatile Cheats cheats;
    private static volatile Ref<EntityStore> owner;
    private static boolean registered;

    static synchronized void register(Cheats c) {
        cheats = c;
        if (registered) return;
        EntityStore.REGISTRY.registerSystem(new DamageFilter());
        registered = true;
    }

    static synchronized void unregister() {
        cheats = null;
        owner = null;
        if (!registered) return;
        registered = false;
        EntityStore.REGISTRY.unregisterSystem(DamageFilter.class);
    }

    // Called every tick with the owner's current ref (it changes on world switch / respawn).
    static void setOwner(Ref<EntityStore> ref) { owner = ref; }

    @Override
    public SystemGroup<EntityStore> getGroup() { return DamageModule.get().getFilterDamageGroup(); }

    @Override
    public Query<EntityStore> getQuery() { return EntityStatMap.getComponentType(); }

    private static boolean same(Ref<EntityStore> a, Ref<EntityStore> b) {
        return a != null && b != null && (a == b || (a.getStore() == b.getStore() && a.getIndex() == b.getIndex()));
    }

    @Override
    public void handle(int index, ArchetypeChunk<EntityStore> chunk, Store<EntityStore> store,
                       CommandBuffer<EntityStore> cb, Damage damage) {
        Cheats c = cheats;
        Ref<EntityStore> me = owner;
        if (c == null || me == null) return;
        try {
            Ref<EntityStore> target = chunk.getReferenceTo(index);
            if (same(target, me)) {
                if (c.god) { damage.setCancelled(true); return; }
                if (c.noFall && DamageCause.FALL != null && damage.getDamageCauseIndex() == DamageCause.getAssetMap().getIndex(DamageCause.FALL.getId()))
                    damage.setCancelled(true);
                return;
            }
            if (damage.getSource() instanceof Damage.EntitySource es && same(es.getRef(), me)) {
                if (c.oneHit) damage.setAmount(1.0e9f);
                else if (c.damageMult != 1f) damage.setAmount(damage.getAmount() * c.damageMult);
            }
        } catch (Throwable ignored) {}
    }
}
