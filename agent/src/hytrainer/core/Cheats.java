// =============================================================================
//  Cheats.java — every feature, applied to the world owner only.
//
//  Threading: the trainer link calls set()/action()/state() from the net thread.
//  set() only stores values; requestTick() posts tick() to the owner's world
//  thread (never more than one in flight), and tick() is the only place game
//  state is read or written. Actions are queued and run inside the next tick.
//
//  Everything changed is tracked so restore() can put it back: components we
//  added, NPCs we froze, movement fields we scaled, forced weather.
// =============================================================================
package hytrainer.core;

import com.hypixel.hytale.component.ArchetypeChunk;
import com.hypixel.hytale.component.ComponentType;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.math.vector.Rotation3f;
import com.hypixel.hytale.math.vector.Transform;
import com.hypixel.hytale.protocol.FlyMode;
import com.hypixel.hytale.protocol.GameMode;
import com.hypixel.hytale.protocol.MovementSettings;
import com.hypixel.hytale.builtin.weather.resources.WeatherResource;
import com.hypixel.hytale.server.core.Options;
import com.hypixel.hytale.server.core.asset.type.attitude.Attitude;
import com.hypixel.hytale.server.core.asset.type.item.config.Item;
import com.hypixel.hytale.server.core.asset.type.weather.config.Weather;
import com.hypixel.hytale.server.core.entity.Frozen;
import com.hypixel.hytale.server.core.entity.InteractionChain;
import com.hypixel.hytale.server.core.entity.InteractionManager;
import com.hypixel.hytale.server.core.modules.interaction.InteractionModule;
import com.hypixel.hytale.server.core.modules.interaction.BlockHarvestUtils;
import com.hypixel.hytale.server.core.universe.world.storage.ChunkStore;
import com.hypixel.hytale.server.core.util.TargetUtil;
import com.hypixel.hytale.component.AddReason;
import com.hypixel.hytale.component.Holder;
import com.hypixel.hytale.server.core.asset.type.blocktype.config.BlockGathering;
import com.hypixel.hytale.server.core.asset.type.blocktype.config.BlockType;
import com.hypixel.hytale.server.core.universe.world.chunk.section.BlockSection;
import com.hypixel.hytale.server.core.entity.entities.ProjectileComponent;
import com.hypixel.hytale.server.core.modules.projectile.config.StandardPhysicsProvider;
import com.hypixel.hytale.server.core.modules.physics.component.Velocity;
import java.util.HashMap;
import org.joml.Vector3i;
import com.hypixel.hytale.server.core.modules.i18n.I18nModule;
import com.hypixel.hytale.server.core.asset.type.item.config.ItemQuality;
import com.hypixel.hytale.protocol.InteractionType;
import com.hypixel.hytale.server.core.entity.effect.EffectControllerComponent;
import com.hypixel.hytale.server.core.entity.entities.Player;
import com.hypixel.hytale.server.core.entity.entities.player.movement.MovementManager;
import com.hypixel.hytale.server.core.inventory.InventoryComponent;
import com.hypixel.hytale.server.core.inventory.ItemStack;
import com.hypixel.hytale.server.core.inventory.container.ItemContainer;
import com.hypixel.hytale.server.core.modules.entity.component.BoundingBox;
import com.hypixel.hytale.server.core.modules.entity.component.HeadRotation;
import com.hypixel.hytale.server.core.modules.entity.component.Invulnerable;
import com.hypixel.hytale.server.core.modules.entity.component.ModelComponent;
import com.hypixel.hytale.server.core.modules.entity.component.TransformComponent;
import com.hypixel.hytale.server.core.modules.entity.damage.Damage;
import com.hypixel.hytale.server.core.modules.entity.damage.DamageCause;
import com.hypixel.hytale.server.core.modules.entity.damage.DamageSystems;
import com.hypixel.hytale.server.core.modules.entity.damage.DeathComponent;
import com.hypixel.hytale.server.core.modules.entity.item.ItemComponent;
import com.hypixel.hytale.server.core.modules.entity.teleport.Teleport;
import com.hypixel.hytale.server.core.modules.entitystats.EntityStatMap;
import com.hypixel.hytale.server.core.modules.entitystats.EntityStatValue;
import com.hypixel.hytale.server.core.modules.entitystats.asset.DefaultEntityStatTypes;
import com.hypixel.hytale.server.core.modules.time.WorldTimeResource;
import com.hypixel.hytale.server.core.universe.PlayerRef;
import com.hypixel.hytale.server.core.universe.Universe;
import com.hypixel.hytale.server.core.universe.world.World;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;
import com.hypixel.hytale.server.npc.entities.NPCEntity;
import com.hypixel.hytale.server.npc.role.support.WorldSupport;
import org.joml.Vector3d;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.Map;
import java.util.TreeMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TreeSet;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Function;

final class Cheats {
    // ---- feature state (written by the net thread, read on the world thread)
    volatile boolean god, infHealth, infStamina, infMana, infOxygen, infAmmo, noFall;
    volatile boolean fly, noClip, infDurability, freezeMobs, freezeTime, oneHit;
    volatile float speed = 1f, jump = 1f, flySpeed = 1f, damageMult = 1f;
    volatile float espRange = 64f;
    volatile boolean espItems = true, espPlayers = true;
    // Combat targets, shared by every combat feature. All creatures are
    // NPCEntity; they are told apart by their attitude towards you. Players
    // only exist when someone is in your world, and then the shared-world
    // rule holds every combat feature back unless the host allows it.
    volatile boolean tgtHostile = true, tgtNeutral, tgtFriendly, tgtPlayers;
    // Timed auto attack, and the features that fire on your own swings.
    volatile boolean autoAttack, aaFrontOnly;
    volatile boolean swingAura, swingReach, ghostHit;
    volatile boolean instantBreak;
    volatile float breakRange = 6f;
    // Drops: pull them to you, and duplicate them.
    volatile boolean vacuum, autoDupe, fastPickup;
    // Your projectiles (arrows, spells, thrown items): steer them, or move them
    // straight onto the target.
    volatile boolean projAim, magicBullet;
    volatile float projRange = 64f, projCone = 25f;
    volatile float vacuumRadius = 12f;
    volatile int dupeMult = 2;
    volatile float aaRadius = 4f, aaRate = 3f, aaDamage = 40f;
    volatile float reachRange = 8f, ghostRange = 8f;
    volatile int auraMax = 1;
    // Shared worlds: when somebody else is in this world, features that change
    // the world for everyone stay off unless the host allows them.
    volatile boolean allowShared;
    volatile String filterError = "";     // set by Core if the damage filter failed to register

    private final UUID owner = Options.getOptionSet().valueOf(Options.OWNER_UUID);
    private final AtomicBoolean busy = new AtomicBoolean();
    private final ConcurrentLinkedQueue<Runnable> actions = new ConcurrentLinkedQueue<>();
    private volatile String state = "player=0";
    private volatile String ents = "";
    private volatile Store<EntityStore> lastStore;     // store the restore pass must run on
    private volatile World lastWorld;
    private Ref<EntityStore> lastRef;           // world thread only

    // ---- what we changed (world thread only)
    private boolean addedInvulnerable, noClipOn;
    private final Set<Ref<EntityStore>> frozen = new HashSet<>();
    private float[] movOrig;                 // baseSpeed, jumpForce, hFly, vFly, fly ordinal
    private boolean weatherForced;
    private double heldDayTime = -1;
    private long ticks;
    private long lastHit;            // auto attack pacing
    private long hits, swings, ghosted, broken;
    private boolean swingWithTool;        // last detected swing was made with a non-weapon tool
    private final Set<Ref<EntityStore>> seenItems = new HashSet<>();   // drops already duplicated (or there before)
    private boolean dupeWasOn;
    private final Map<Ref<EntityStore>, Ref<EntityStore>> projTargets = new HashMap<>();
    private final Set<Ref<EntityStore>> projPlaced = new HashSet<>();
    private long homed, bullets;
    private final Set<Ref<EntityStore>> thrown = new HashSet<>();   // drops you threw: normal delay
    private static java.lang.reflect.Field fDelay, fThrottle;        // ItemComponent has no setter for the throttle
    private long duped, pulled;
    private final Set<Integer> seenChains = new HashSet<>();
    private volatile int playerCount = 1;
    private volatile boolean freezeMobsEff;   // freezeMobs with the shared-world rule applied

    // ---------------------------------------------------------------------
    //  Net-thread API
    // ---------------------------------------------------------------------
    boolean set(String k, String v) {
        boolean b = v.equals("1");
        float f;
        try { f = Float.parseFloat(v); } catch (NumberFormatException e) { f = 0f; }
        switch (k) {
            case "god" -> god = b;
            case "infhp" -> infHealth = b;
            case "infstam" -> infStamina = b;
            case "infmana" -> infMana = b;
            case "infoxy" -> infOxygen = b;
            case "infammo" -> infAmmo = b;
            case "nofall" -> noFall = b;
            case "fly" -> fly = b;
            case "noclip" -> noClip = b;
            case "infdur" -> infDurability = b;
            case "freezemobs" -> freezeMobs = b;
            case "freezetime" -> freezeTime = b;
            case "onehit" -> oneHit = b;
            case "speed" -> speed = clamp(f, 0.1f, 10f);
            case "jump" -> jump = clamp(f, 0.1f, 10f);
            case "flyspeed" -> flySpeed = clamp(f, 0.1f, 10f);
            case "dmgmult" -> damageMult = clamp(f, 0f, 1000f);
            case "esprange" -> espRange = clamp(f, 8f, 512f);
            case "espitems" -> espItems = b;
            case "espplayers" -> espPlayers = b;
            case "allowshared" -> allowShared = b;
            case "autoattack" -> autoAttack = b;
            case "tgthostile" -> tgtHostile = b;
            case "tgtneutral" -> tgtNeutral = b;
            case "tgtfriendly" -> tgtFriendly = b;
            case "tgtplayers" -> tgtPlayers = b;
            case "swingaura" -> swingAura = b;
            case "swingreach" -> swingReach = b;
            case "ghosthit" -> ghostHit = b;
            case "reachrange" -> reachRange = clamp(f, 2f, 32f);
            case "ghostrange" -> ghostRange = clamp(f, 2f, 32f);
            case "auramax" -> auraMax = (int) clamp(f, 1f, 16f);
            case "instabreak" -> instantBreak = b;
            case "vacuum" -> vacuum = b;
            case "vacuumradius" -> vacuumRadius = clamp(f, 2f, 64f);
            case "autodupe" -> autoDupe = b;
            case "fastpickup" -> fastPickup = b;
            case "projaim" -> projAim = b;
            case "magicbullet" -> magicBullet = b;
            case "projrange" -> projRange = clamp(f, 8f, 256f);
            case "projcone" -> projCone = clamp(f, 3f, 180f);
            case "dupemult" -> dupeMult = (int) clamp(f, 2f, 16f);
            case "breakrange" -> breakRange = clamp(f, 2f, 32f);
            case "aafront" -> aaFrontOnly = b;
            case "aaradius" -> aaRadius = clamp(f, 1f, 16f);
            case "aarate" -> aaRate = clamp(f, 0.2f, 20f);
            case "aadamage" -> aaDamage = clamp(f, 1f, 10000f);
            default -> { return false; }
        }
        return true;
    }

    String state() { return state; }
    String entities() { return ents; }

    // Every item asset, with what the browser needs to group and show it:
    //   id <US> English name <US> category <US> sub-category <US> type <US> quality <US> max stack
    String items() {
        I18nModule i18n = null;
        try { i18n = I18nModule.get(); } catch (Throwable ignored) {}
        StringBuilder sb = new StringBuilder(1 << 19);
        for (Map.Entry<String, Item> e : new TreeMap<>(Item.getAssetMap().getAssetMap()).entrySet()) {
            Item it = e.getValue();
            String id = e.getKey();
            if (it == null) continue;
            String name = null;
            try {
                String key = it.getTranslationKey();
                if (i18n != null && key != null) name = i18n.getMessage("en-US", key);
                if (name != null && name.equals(key)) name = null;
            } catch (Throwable ignored) {}
            if (name == null || name.isBlank()) name = pretty(id);
            String cat = "", subCat = "", quality = "";
            try { String[] c = it.getCategories(); if (c != null && c.length > 0 && c[0] != null) cat = c[0]; } catch (Throwable ignored) {}
            try { if (it.getSubCategory() != null) subCat = it.getSubCategory(); } catch (Throwable ignored) {}
            try { ItemQuality q = ItemQuality.getAssetMap().getAsset(it.getQualityIndex()); if (q != null) quality = q.getId(); } catch (Throwable ignored) {}
            String kind;
            try {
                kind = it.getWeapon() != null ? "Weapons" : it.getTool() != null ? "Tools" : it.getArmor() != null ? "Armor"
                     : it.getGlider() != null ? "Gliders" : it.getBlockId() != null ? "Blocks" : it.getUtility() != null ? "Utility"
                     : it.getPortalKey() != null ? "Keys" : "Items";
            } catch (Throwable t) { kind = "Items"; }
            if (!sb.isEmpty()) sb.append('\t');
            sb.append(clean(id)).append('\u001f').append(clean(name)).append('\u001f').append(clean(cat)).append('\u001f')
              .append(clean(subCat)).append('\u001f').append(kind).append('\u001f').append(clean(quality)).append('\u001f')
              .append(Math.max(1, it.getMaxStack()));
        }
        return sb.toString();
    }

    private static String clean(String v) {
        return v == null ? "" : v.replace('\t', ' ').replace('\u001f', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    String weathers() {
        TreeSet<String> ids = new TreeSet<>(Weather.getAssetMap().getAssetMap().keySet());
        return String.join("\t", ids);
    }

    // Runs inside the next tick; the reply waits for it (bounded).
    String action(String name, String args) {
        CompletableFuture<String> done = new CompletableFuture<>();
        actions.add(() -> {
            try { done.complete(doAction(name, args.isEmpty() ? new String[0] : args.split(" "))); }
            catch (Throwable t) { done.complete("ERR " + t.getClass().getSimpleName() + ": " + t.getMessage()); }
        });
        try { return done.get(3, TimeUnit.SECONDS); }
        catch (Exception e) { return "ERR world not responding"; }
    }

    // ---------------------------------------------------------------------
    //  Scheduling
    // ---------------------------------------------------------------------
    private Ref<EntityStore> ownerRef() {
        PlayerRef p = owner != null ? Universe.get().getPlayer(owner) : null;
        Ref<EntityStore> r = p != null ? p.getReference() : null;
        return r != null && r.isValid() ? r : null;
    }

    void requestTick() {
        try {
            Ref<EntityStore> ref = ownerRef();
            if (ref == null) { state = "player=0"; ents = ""; return; }
            World world = ref.getStore().getExternalData().getWorld();
            if (world == null || !world.isAlive()) return;
            if (!busy.compareAndSet(false, true)) return;
            try {
                world.execute(() -> {
                    try { tick(world); } catch (Throwable ignored) {} finally { busy.set(false); }
                });
            } catch (Throwable t) { busy.set(false); }
        } catch (Throwable ignored) {}
    }

    // ---------------------------------------------------------------------
    //  World thread
    // ---------------------------------------------------------------------
    private void tick(World world) {
        Ref<EntityStore> ref = ownerRef();
        if (ref == null) return;
        Store<EntityStore> store = ref.getStore();
        if (!store.isInThread()) return;              // player changed worlds since posting
        if (lastStore != null && lastStore != store) forgetWorld();
        lastStore = store; lastWorld = world;
        if (ref != lastRef) {                  // respawned: a new entity, nothing applied to it yet
            lastRef = ref;
            noClipOn = false;
            seenChains.clear();
        }
        ticks++;
        try { playerCount = Universe.get().getPlayerCount(); } catch (Throwable ignored) {}
        freezeMobsEff = freezeMobs && !sharedLocked();

        for (Runnable a; (a = actions.poll()) != null; ) a.run();

        DamageFilter.setOwner(ref);
        applyStats(ref, store);
        applyInvulnerable(ref, store);
        applyMovement(ref, store);
        applyNoClip(ref, store);
        if (infDurability && ticks % 10 == 0) repairAll(ref, store, true);
        applyFreezeTime(world, store);
        if (detectSwing(ref, store)) onSwing(ref, store);
        applyDrops(ref, store);
        applyProjectiles(ref, store);
        autoAttack(ref, store);
        collect(ref, store, world);
    }

    // A world switch drops our per-entity bookkeeping (those refs are gone).
    private void forgetWorld() {
        frozen.clear();
        seenItems.clear();
        thrown.clear();
        projTargets.clear();
        projPlaced.clear();
        addedInvulnerable = false;
        heldDayTime = -1;
        weatherForced = false;
    }

    private void applyStats(Ref<EntityStore> ref, Store<EntityStore> store) {
        EntityStatMap m = store.getComponent(ref, EntityStatMap.getComponentType());
        if (m == null || store.getArchetype(ref).contains(DeathComponent.getComponentType())) return;
        if (god || infHealth) top(m, DefaultEntityStatTypes.getHealth());
        if (infStamina) top(m, DefaultEntityStatTypes.getStamina());
        if (infMana) top(m, DefaultEntityStatTypes.getMana());
        if (infOxygen) top(m, DefaultEntityStatTypes.getOxygen());
        if (infAmmo) top(m, DefaultEntityStatTypes.getAmmo());
    }

    private static void top(EntityStatMap m, int idx) {
        if (idx == Integer.MIN_VALUE) return;
        EntityStatValue v = m.get(idx);
        if (v != null && v.get() < v.getMax()) m.maximizeStatValue(idx);
    }

    private void applyInvulnerable(Ref<EntityStore> ref, Store<EntityStore> store) {
        ComponentType<EntityStore, Invulnerable> t = Invulnerable.getComponentType();
        boolean has = store.getArchetype(ref).contains(t);
        if (god && !has) { store.ensureComponent(ref, t); addedInvulnerable = true; }
        else if (!god && addedInvulnerable) { if (has) store.tryRemoveComponent(ref, t); addedInvulnerable = false; }
    }

    private void applyMovement(Ref<EntityStore> ref, Store<EntityStore> store) {
        MovementManager mm = store.getComponent(ref, MovementManager.getComponentType());
        PlayerRef pr = store.getComponent(ref, PlayerRef.getComponentType());
        if (mm == null || pr == null || mm.getSettings() == null || mm.getDefaultSettings() == null) return;
        MovementSettings s = mm.getSettings(), d = mm.getDefaultSettings();
        boolean active = speed != 1f || jump != 1f || flySpeed != 1f || fly;
        if (!active) {
            if (movOrig != null) {          // put back what we changed
                s.baseSpeed = d.baseSpeed; s.jumpForce = d.jumpForce;
                s.horizontalFlySpeed = d.horizontalFlySpeed; s.verticalFlySpeed = d.verticalFlySpeed;
                s.fly = d.fly;
                movOrig = null;
                mm.update(pr.getPacketHandler());
            }
            return;
        }
        movOrig = new float[0];
        float bs = d.baseSpeed * speed, jf = d.jumpForce * jump;
        float hf = d.horizontalFlySpeed * flySpeed, vf = d.verticalFlySpeed * flySpeed;
        FlyMode fm = fly ? FlyMode.Allowed : d.fly;
        if (s.baseSpeed != bs || s.jumpForce != jf || s.horizontalFlySpeed != hf || s.verticalFlySpeed != vf || s.fly != fm) {
            s.baseSpeed = bs; s.jumpForce = jf; s.horizontalFlySpeed = hf; s.verticalFlySpeed = vf; s.fly = fm;
            mm.update(pr.getPacketHandler());
        }
    }

    private void applyNoClip(Ref<EntityStore> ref, Store<EntityStore> store) {
        if (noClip == noClipOn) return;
        Player p = store.getComponent(ref, Player.getComponentType());
        if (p == null) return;
        p.setNoClip(ref, noClip, false, store);
        noClipOn = noClip;
    }

    // Somebody else is in this world and the host has not allowed features
    // that change it for everyone.
    private boolean sharedLocked() { return playerCount > 1 && !allowShared; }

    private void applyFreezeTime(World world, Store<EntityStore> store) {
        WorldTimeResource t = store.getResource(WorldTimeResource.getResourceType());
        if (t == null) return;
        if (!freezeTime || sharedLocked()) { heldDayTime = -1; return; }
        if (heldDayTime < 0) heldDayTime = t.getDayProgress();
        if (ticks % 20 == 0 && Math.abs(t.getDayProgress() - heldDayTime) > 0.0005) t.setDayTime(heldDayTime, world, store);
    }

    // Live NPCs near the owner; freezing is applied in the same pass.
    private void collect(Ref<EntityStore> ref, Store<EntityStore> store, World world) {
        TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
        if (tc == null) return;
        Vector3d me = tc.getPosition();
        HeadRotation hr = store.getComponent(ref, HeadRotation.getComponentType());
        Rotation3f rot = hr != null ? hr.getRotation() : tc.getRotation();
        float eye = ModelComponent.getEyeHeight(ref, store);
        EntityStatMap m = store.getComponent(ref, EntityStatMap.getComponentType());
        Player pl = store.getComponent(ref, Player.getComponentType());
        WorldTimeResource time = store.getResource(WorldTimeResource.getResourceType());

        double range = espRange, r2 = range * range;
        StringBuilder sb = new StringBuilder(4096);
        int[] count = {0};
        List<Ref<EntityStore>> seenFrozen = new ArrayList<>();
        store.forEachChunk(NPCEntity.getComponentType(), (chunk, cb) -> {
            for (int i = 0; i < chunk.size(); i++) {
                TransformComponent t = chunk.getComponent(i, TransformComponent.getComponentType());
                if (t == null) continue;
                Vector3d p = t.getPosition();
                double dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                Ref<EntityStore> nr = chunk.getReferenceTo(i);
                boolean dead = chunk.getArchetype().contains(DeathComponent.getComponentType());
                if (freezeMobsEff && !dead) seenFrozen.add(nr);
                NPCEntity npc = chunk.getComponent(i, NPCEntity.getComponentType());
                EntityStatMap sm = chunk.getComponent(i, EntityStatMap.getComponentType());
                EntityStatValue hp = sm != null ? sm.get(DefaultEntityStatTypes.getHealth()) : null;
                WorldSupport ws = chunk.getComponent(i, WorldSupport.getComponentType());
                Attitude att = ws != null ? ws.getDefaultPlayerAttitude() : null;
                if (dead) continue;
                entity(sb, nr.getIndex(), att == Attitude.HOSTILE ? 'H' : att == Attitude.FRIENDLY || att == Attitude.REVERED ? 'F' : 'N',
                       p, height(chunk, i), hp != null ? hp.get() : 0f, hp != null ? hp.getMax() : 0f,
                       npc != null ? pretty(npc.getRoleName()) : "?");
                count[0]++;
            }
        });
        if (espItems) {
            store.forEachChunk(ItemComponent.getComponentType(), (chunk, cb) -> {
                for (int i = 0; i < chunk.size(); i++) {
                    TransformComponent t = chunk.getComponent(i, TransformComponent.getComponentType());
                    ItemComponent ic = chunk.getComponent(i, ItemComponent.getComponentType());
                    if (t == null || ic == null || ic.getItemStack() == null) continue;
                    Vector3d p = t.getPosition();
                    double dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
                    if (dx * dx + dy * dy + dz * dz > r2) continue;
                    ItemStack st = ic.getItemStack();
                    entity(sb, chunk.getReferenceTo(i).getIndex(), 'I', p, 0.4f, st.getQuantity(), 0f, pretty(st.getItemId()));
                }
            });
        }
        // Other players in this world. Nothing here targets them: the ESP
        // only draws, and auto attack never looks at players.
        if (espPlayers) {
            try {
                for (PlayerRef other : Universe.get().getPlayers()) {
                    if (other == null) continue;
                    if (owner != null && owner.equals(other.getUuid())) continue;
                    Ref<EntityStore> r = other.getReference();
                    if (r == null || !r.isValid() || r.getStore() != store) continue;   // another world
                    if (store.getArchetype(r).contains(DeathComponent.getComponentType())) continue;
                    TransformComponent t = store.getComponent(r, TransformComponent.getComponentType());
                    if (t == null) continue;
                    Vector3d p = t.getPosition();
                    double dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
                    if (dx * dx + dy * dy + dz * dz > r2) continue;
                    EntityStatMap sm = store.getComponent(r, EntityStatMap.getComponentType());
                    EntityStatValue hp = sm != null ? sm.get(DefaultEntityStatTypes.getHealth()) : null;
                    entity(sb, r.getIndex(), 'P', p, height(store, r),
                           hp != null ? hp.get() : 0f, hp != null ? hp.getMax() : 0f,
                           other.getUsername() == null ? "player" : other.getUsername());
                }
            } catch (Throwable ignored) {}
        }
        applyFreezeMobs(store, seenFrozen);
        ents = sb.toString();

        StringBuilder s = new StringBuilder(256);
        kv(s, "player", "1");
        kv(s, "seq", ticks);
        kv(s, "x", me.x); kv(s, "y", me.y); kv(s, "z", me.z);
        kv(s, "pitch", rot.pitch()); kv(s, "yaw", rot.yaw()); kv(s, "eye", eye);
        if (m != null) {
            stat(s, m, "hp", DefaultEntityStatTypes.getHealth());
            stat(s, m, "stam", DefaultEntityStatTypes.getStamina());
            stat(s, m, "mana", DefaultEntityStatTypes.getMana());
            stat(s, m, "oxy", DefaultEntityStatTypes.getOxygen());
        }
        if (pl != null) kv(s, "mode", pl.getGameMode().name());
        if (time != null) kv(s, "day", time.getDayProgress());
        kv(s, "world", world.getName());
        kv(s, "npcs", count[0]);
        kv(s, "players", playerCount);
        kv(s, "shared", sharedLocked() ? 1 : 0);
        kv(s, "aahits", hits);
        kv(s, "swings", swings);
        kv(s, "ghosted", ghosted);
        kv(s, "broken", broken);
        kv(s, "duped", duped);
        kv(s, "pulled", pulled);
        kv(s, "homed", homed);
        kv(s, "bullets", bullets);
        kv(s, "filter", filterError.isEmpty() ? 1 : 0);
        state = s.toString();
    }

    private void applyFreezeMobs(Store<EntityStore> store, List<Ref<EntityStore>> inRange) {
        ComponentType<EntityStore, Frozen> ft = Frozen.getComponentType();
        if (freezeMobsEff) {
            for (Ref<EntityStore> r : inRange) {
                if (frozen.contains(r) || store.getArchetype(r).contains(ft)) continue;   // already frozen by the game: not ours
                store.ensureComponent(r, ft);
                frozen.add(r);
            }
        }
        if (!freezeMobsEff && !frozen.isEmpty()) unfreezeAll(store);
        frozen.removeIf(r -> !r.isValid());
    }

    private void unfreezeAll(Store<EntityStore> store) {
        for (Ref<EntityStore> r : frozen) if (r.isValid()) store.tryRemoveComponent(r, Frozen.getComponentType());
        frozen.clear();
    }

    private static float height(Store<EntityStore> store, Ref<EntityStore> ref) {
        BoundingBox bb = store.getComponent(ref, BoundingBox.getComponentType());
        if (bb == null || bb.getBoundingBox() == null) return 1.8f;
        double h = bb.getBoundingBox().max.y - bb.getBoundingBox().min.y;
        return h > 0.05 && h < 50 ? (float) h : 1.8f;
    }

    private static float height(ArchetypeChunk<EntityStore> chunk, int i) {
        BoundingBox bb = chunk.getComponent(i, BoundingBox.getComponentType());
        if (bb == null || bb.getBoundingBox() == null) return 1.8f;
        double h = bb.getBoundingBox().max.y - bb.getBoundingBox().min.y;
        return h > 0.05 && h < 50 ? (float) h : 1.8f;
    }

    private static void entity(StringBuilder sb, int id, char type, Vector3d p, float h, float hp, float max, String name) {
        if (!sb.isEmpty()) sb.append('\t');
        sb.append(type).append(',').append(id).append(',').append(fmt(p.x)).append(',').append(fmt(p.y)).append(',').append(fmt(p.z))
          .append(',').append(fmt(h)).append(',').append(fmt(hp)).append(',').append(fmt(max)).append(',')
          .append(name.replace(',', ' ').replace('\t', ' '));
    }

    private static void stat(StringBuilder s, EntityStatMap m, String k, int idx) {
        if (idx == Integer.MIN_VALUE) return;
        EntityStatValue v = m.get(idx);
        if (v == null) return;
        kv(s, k, v.get()); kv(s, k + "max", v.getMax());
    }

    private static void kv(StringBuilder s, String k, Object v) {
        if (!s.isEmpty()) s.append('\t');
        s.append(k).append('=').append(v instanceof Double || v instanceof Float ? fmt(((Number) v).doubleValue()) : String.valueOf(v));
    }

    private static String fmt(double v) { return String.format(Locale.ROOT, "%.3f", v); }

    // "Trork_Warrior" -> "Trork Warrior"
    private static String pretty(String id) {
        if (id == null) return "?";
        int c = id.lastIndexOf(':');
        return (c >= 0 ? id.substring(c + 1) : id).replace('_', ' ');
    }

    private static float clamp(float v, float lo, float hi) { return Math.max(lo, Math.min(hi, v)); }

    // ---------------------------------------------------------------------
    //  One-shot actions (world thread)
    // ---------------------------------------------------------------------
    private String doAction(String name, String[] a) {
        Ref<EntityStore> ref = ownerRef();
        if (ref == null) return "ERR no player";
        Store<EntityStore> store = ref.getStore();
        World world = store.getExternalData().getWorld();
        if (sharedLocked() && (name.equals("time") || name.equals("weather") || name.equals("killmobs")))
            return "ERR other players are in this world";
        switch (name) {
            case "heal" -> {
                EntityStatMap m = store.getComponent(ref, EntityStatMap.getComponentType());
                if (m == null) return "ERR no stats";
                for (int idx : new int[]{ DefaultEntityStatTypes.getHealth(), DefaultEntityStatTypes.getStamina(),
                                          DefaultEntityStatTypes.getMana(), DefaultEntityStatTypes.getOxygen() })
                    if (idx != Integer.MIN_VALUE && m.get(idx) != null) m.maximizeStatValue(idx);
                return "OK healed";
            }
            case "clearfx" -> {
                EffectControllerComponent e = store.getComponent(ref, EffectControllerComponent.getComponentType());
                if (e != null) e.clearEffects(ref, store);
                return "OK effects cleared";
            }
            case "gamemode" -> {
                GameMode gm = a.length > 0 && a[0].equalsIgnoreCase("creative") ? GameMode.Creative : GameMode.Adventure;
                Player.setGameMode(ref, gm, store);
                return "OK " + gm.name();
            }
            case "time" -> {
                double h = Double.parseDouble(a[0]);
                WorldTimeResource t = store.getResource(WorldTimeResource.getResourceType());
                double dp = Math.max(0, Math.min(0.9999, h / WorldTimeResource.HOURS_PER_DAY));
                t.setDayTime(dp, world, store);
                if (heldDayTime >= 0) heldDayTime = dp;
                return "OK time set";
            }
            case "weather" -> {
                WeatherResource w = store.getResource(WeatherResource.getResourceType());
                if (w == null) return "ERR no weather";
                String id = a.length > 0 && !a[0].equals("clear") ? a[0] : null;
                w.setForcedWeather(id);            // runtime only; the world config is left alone
                weatherForced = id != null;
                return "OK weather " + (id == null ? "natural" : id);
            }
            case "tp" -> {
                teleport(ref, store, new Vector3d(Double.parseDouble(a[0]), Double.parseDouble(a[1]), Double.parseDouble(a[2])));
                return "OK teleported";
            }
            case "tpfwd" -> {
                double dist = Double.parseDouble(a[0]);
                TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
                HeadRotation hr = store.getComponent(ref, HeadRotation.getComponentType());
                Rotation3f r = hr != null ? hr.getRotation() : tc.getRotation();
                Vector3d dir = Transform.getDirection(r.pitch(), r.yaw());
                Vector3d p = new Vector3d(tc.getPosition()).add(dir.x * dist, Math.max(0, dir.y) * dist, dir.z * dist);
                teleport(ref, store, p);
                return "OK teleported";
            }
            case "tpup" -> {
                TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
                teleport(ref, store, new Vector3d(tc.getPosition()).add(0, Double.parseDouble(a[0]), 0));
                return "OK teleported";
            }
            case "tpspawn" -> {
                Transform[] sp = world.getWorldConfig().getSpawnProvider().getSpawnPoints();
                if (sp == null || sp.length == 0) return "ERR no spawn point";
                teleport(ref, store, new Vector3d(sp[0].getPosition()));
                return "OK at spawn";
            }
            case "give" -> {
                String id = a[0];
                int qty = a.length > 1 ? Integer.parseInt(a[1]) : 1;
                Item item = Item.getAssetMap().getAsset(id);
                if (item == null) return "ERR unknown item";
                qty = Math.max(1, Math.min(qty, 9999));
                int left = qty;
                while (left > 0) {
                    int n = Math.min(left, Math.max(1, item.getMaxStack()));
                    ItemStack rem = Player.giveItem(new ItemStack(id, n, null).withDurability(Double.MAX_VALUE), ref, store).getRemainder();
                    if (rem != null && !rem.isEmpty()) return "ERR inventory full (" + (qty - left + n - rem.getQuantity()) + " given)";
                    left -= n;
                }
                return "OK gave " + qty;
            }
            case "repair" -> { return "OK repaired " + repairAll(ref, store, false); }
            case "dupedrops" -> {
                double r = a.length > 0 ? Double.parseDouble(a[0]) : 16;
                int mult = a.length > 1 ? Math.max(2, Math.min(16, Integer.parseInt(a[1]))) : 2;
                Vector3d me = store.getComponent(ref, TransformComponent.getComponentType()).getPosition();
                List<ItemStack> stacks = new ArrayList<>();
                List<Vector3d> where = new ArrayList<>();
                collectDrops(store, me, r * r, null, stacks, where, null);
                int made = duplicate(store, stacks, where, mult);
                return "OK duplicated " + stacks.size() + " drops (" + made + " copies)";
            }
            case "killmobs" -> {
                double r = a.length > 0 ? Double.parseDouble(a[0]) : 48;
                boolean hostileOnly = a.length < 2 || !a[1].equals("all");
                return "OK killed " + killNear(ref, store, r, hostileOnly);
            }
            default -> { return "ERR unknown action"; }
        }
    }

    // ---------------------------------------------------------------------
    //  Auto attack
    //
    //  Hits the nearest target in reach at a fixed rate, as a swing from the
    //  owner: the damage filter above applies One-Hit Kills and the damage
    //  multiplier, and kill credit and drops behave like a normal kill.
    //  Which entities count is up to targets() and the target toggles.
    // ---------------------------------------------------------------------
    // ---------------------------------------------------------------------
    //  Targets
    // ---------------------------------------------------------------------
    private record Target(Ref<EntityStore> ref, Vector3d pos, boolean player) {}

    // Live targets within radius of `me` that pass the target toggles.
    private List<Target> targets(Ref<EntityStore> self, Store<EntityStore> store, Vector3d me, double radius) {
        List<Target> out = new ArrayList<>();
        double r2 = radius * radius;
        boolean h = tgtHostile, n = tgtNeutral, f = tgtFriendly;
        if (h || n || f) {
            store.forEachChunk(NPCEntity.getComponentType(), (chunk, cb) -> {
                if (chunk.getArchetype().contains(DeathComponent.getComponentType())) return;
                for (int i = 0; i < chunk.size(); i++) {
                    TransformComponent t = chunk.getComponent(i, TransformComponent.getComponentType());
                    if (t == null || t.getPosition().distanceSquared(me) > r2) continue;
                    WorldSupport ws = chunk.getComponent(i, WorldSupport.getComponentType());
                    Attitude att = ws != null ? ws.getDefaultPlayerAttitude() : null;
                    boolean ok = att == Attitude.HOSTILE ? h : att == Attitude.FRIENDLY || att == Attitude.REVERED ? f : n;
                    if (ok) out.add(new Target(chunk.getReferenceTo(i), new Vector3d(t.getPosition()), false));
                }
            });
        }
        if (tgtPlayers) {
            try {
                for (PlayerRef other : Universe.get().getPlayers()) {
                    if (other == null || (owner != null && owner.equals(other.getUuid()))) continue;
                    Ref<EntityStore> r = other.getReference();
                    if (r == null || !r.isValid() || r.getStore() != store || r == self) continue;
                    if (store.getArchetype(r).contains(DeathComponent.getComponentType())) continue;
                    TransformComponent t = store.getComponent(r, TransformComponent.getComponentType());
                    if (t == null || t.getPosition().distanceSquared(me) > r2) continue;
                    out.add(new Target(r, new Vector3d(t.getPosition()), true));
                }
            } catch (Throwable ignored) {}
        }
        return out;
    }

    // A hit from you: One-Hit Kills and the damage multiplier apply (DamageFilter),
    // and kill credit and drops behave like a normal kill.
    private void hit(Ref<EntityStore> self, Store<EntityStore> store, Ref<EntityStore> target) {
        if (target == null || !target.isValid()) return;
        hits++;
        DamageSystems.executeDamage(target, store, new Damage(new Damage.EntitySource(self), DamageCause.PHYSICAL, aaDamage));
    }

    // ---------------------------------------------------------------------
    //  Swings
    //
    //  Every attack you make starts a Primary interaction chain on your
    //  InteractionManager. A chain id not seen last tick is a new swing. Swings
    //  with a tool that is not a weapon (mining, chopping) are ignored.
    // ---------------------------------------------------------------------
    private boolean detectSwing(Ref<EntityStore> ref, Store<EntityStore> store) {
        if (!(swingAura || swingReach || ghostHit || instantBreak)) { seenChains.clear(); return false; }
        InteractionManager im;
        try { im = store.getComponent(ref, InteractionModule.get().getInteractionManagerComponent()); }
        catch (Throwable t) { return false; }
        if (im == null) return false;
        boolean swing = false;
        Set<Integer> now = new HashSet<>();
        for (InteractionChain c : im.getChains().values()) {
            if (c == null) continue;
            now.add(c.getChainId());
            if (!seenChains.contains(c.getChainId()) && c.getType() == InteractionType.Primary) swing = true;
        }
        seenChains.clear();
        seenChains.addAll(now);
        if (!swing) return false;
        swingWithTool = false;
        try {
            ItemStack held = InventoryComponent.getItemInHand(store, ref);
            if (held != null && !held.isEmpty()) {
                Item it = held.getItem();
                swingWithTool = it != null && it.getTool() != null && it.getWeapon() == null;
            }
        } catch (Throwable ignored) {}
        return true;
    }

    private void onSwing(Ref<EntityStore> ref, Store<EntityStore> store) {
        swings++;
        boolean struck = !swingWithTool && !sharedLocked() && combatSwing(ref, store);
        // Instant break: the block you are looking at breaks on the swing,
        // through the game's own break path (drops, tool durability). Skipped
        // when the swing just hit a creature, so fighting does not dig.
        if (instantBreak && !struck) breakTarget(ref, store);
    }

    private void breakTarget(Ref<EntityStore> ref, Store<EntityStore> store) {
        TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
        if (tc == null) return;
        HeadRotation hr = store.getComponent(ref, HeadRotation.getComponentType());
        Rotation3f rot = hr != null ? hr.getRotation() : tc.getRotation();
        Vector3d dir = Transform.getDirection(rot.pitch(), rot.yaw());
        Vector3d eye = new Vector3d(tc.getPosition()).add(0, ModelComponent.getEyeHeight(ref, store), 0);
        World world = store.getExternalData().getWorld();
        ChunkStore cs = world.getChunkStore();
        Vector3i blk = TargetUtil.getTargetBlock(cs, (id, fluid) -> id != 0, eye.x, eye.y, eye.z, dir.x, dir.y, dir.z, breakRange);
        if (blk == null) return;
        Ref<ChunkStore> sec = cs.getChunkSectionReferenceAtBlock(blk.x, blk.y, blk.z);
        if (sec == null || !sec.isValid()) return;
        Store<ChunkStore> chunks = cs.getStore();
        BlockSection bs0 = chunks.getComponent(sec, BlockSection.getComponentType());
        BlockType target = bs0 != null ? BlockType.getAssetMap().getAsset(bs0.get(blk.x, blk.y, blk.z)) : null;
        // The world's own rule, as the game's break interaction checks it.
        try { if (!world.getGameplayConfig().getWorldConfig().isBlockBreakingAllowed(target)) return; } catch (Throwable ignored) {}
        ItemStack held = null;
        try { held = InventoryComponent.getItemInHand(store, ref); } catch (Throwable ignored) {}

        // 1) The survival break path with overwhelming damage: one hit, the
        //    block's normal drops, tool requirements and tool wear.
        BlockHarvestUtils.performBlockDamage(ref, ref, blk, held, null, null, false, 100000f, 0, false, sec, store, chunks);

        // 2) Still standing: fists and the wrong tool do no damage at all in
        //    survival. Break it outright, with the block's own drop list.
        BlockSection bs = chunks.getComponent(sec, BlockSection.getComponentType());
        int id = bs != null ? bs.get(blk.x, blk.y, blk.z) : 0;
        if (id != 0) {
            BlockType bt = BlockType.getAssetMap().getAsset(id);
            if (bt == null || bt.isUnknown() || !mineable(bt)) return;     // e.g. "Unbreakable" blocks
            int qty = 1;
            String itemId = null, list = null;
            BlockGathering g = bt.getGathering();
            if (g != null) {
                if (g.isSoft() && g.getSoft() != null) { itemId = g.getSoft().getItemId(); list = g.getSoft().getDropListId(); }
                else if (g.getBreaking() != null) {
                    qty = Math.max(1, g.getBreaking().getQuantity());
                    itemId = g.getBreaking().getItemId();
                    list = g.getBreaking().getDropListId();
                }
            }
            BlockHarvestUtils.performBlockBreak(blk, bt, held, qty, itemId, list, 0, ref, sec, store, chunks);
            id = bs.get(blk.x, blk.y, blk.z);
        }
        if (id == 0) broken++;
    }

    // A block some tool in the game can mine. Blocks whose gather type no
    // tool has (the "Unbreakable" type: the world floor, sealed coffers, ...)
    // are left alone by the fallback break.
    private static Set<String> mineableTypes;
    private static boolean mineable(BlockType bt) {
        BlockGathering g = bt.getGathering();
        if (g == null) return false;
        if (g.isSoft()) return true;
        if (g.getBreaking() == null || g.getBreaking().getGatherType() == null) return false;
        if (mineableTypes == null) {
            Set<String> set = new HashSet<>();
            for (Item it : Item.getAssetMap().getAssetMap().values()) {
                try {
                    if (it == null || it.getTool() == null || it.getTool().getSpecs() == null) continue;
                    for (var spec : it.getTool().getSpecs())
                        if (spec != null && spec.getGatherType() != null && spec.getPower() > 0) set.add(spec.getGatherType());
                } catch (Throwable ignored) {}
            }
            mineableTypes = set;
        }
        return mineableTypes.contains(g.getBreaking().getGatherType());
    }

    // ---------------------------------------------------------------------
    //  Drops
    // ---------------------------------------------------------------------
    // Item entities within sqrt(r2) of `me`. fresh (optional) collects only
    // the ones not seen before and marks them seen.
    private void collectDrops(Store<EntityStore> store, Vector3d me, double r2, List<Ref<EntityStore>> all,
                              List<ItemStack> stacks, List<Vector3d> where, Set<Ref<EntityStore>> seen) {
        store.forEachChunk(ItemComponent.getComponentType(), (chunk, cb) -> {
            for (int i = 0; i < chunk.size(); i++) {
                TransformComponent t = chunk.getComponent(i, TransformComponent.getComponentType());
                ItemComponent ic = chunk.getComponent(i, ItemComponent.getComponentType());
                if (t == null || ic == null || ic.getItemStack() == null || ic.getItemStack().isEmpty()) continue;
                if (t.getPosition().distanceSquared(me) > r2) continue;
                Ref<EntityStore> r = chunk.getReferenceTo(i);
                if (all != null) all.add(r);
                if (stacks != null && (seen == null || seen.add(r))) {
                    stacks.add(ic.getItemStack());
                    where.add(new Vector3d(t.getPosition()));
                }
            }
        });
    }

    // Spawns (mult - 1) more of each stack where it lies, through the game's own
    // drop path. Returns how many item entities were made.
    private int duplicate(Store<EntityStore> store, List<ItemStack> stacks, List<Vector3d> where, int mult) {
        int made = 0;
        for (int i = 0; i < stacks.size(); i++) {
            for (int k = 1; k < mult; k++) {
                Holder<EntityStore>[] h = ItemComponent.generateItemDrops(store, List.of(stacks.get(i)), where.get(i), Rotation3f.IDENTITY);
                Ref<EntityStore>[] refs = store.addEntities(h, AddReason.SPAWN);
                for (Ref<EntityStore> r : refs) if (r != null) seenItems.add(r);   // copies are never copied again
                made += refs.length;
            }
        }
        duped += made;
        return made;
    }

    // How the game marks drops (ItemComponent / ItemUtils):
    //   - a freshly spawned drop has a pickup delay counting down: 0.5 s for
    //     block and creature drops, 1.5 s for items a player throws;
    //   - drops that were already lying around, or come in with a loading
    //     chunk, have no delay left.
    // Every drop is looked at once, the first tick it exists. Fresh ones may
    // be duplicated; thrown ones are left out of Instant Pickup and the Vacuum,
    // or you could never drop anything.
    private boolean ensureDelayFields() {
        if (fDelay != null) return true;
        try {
            fDelay = ItemComponent.class.getDeclaredField("pickupDelay");
            fThrottle = ItemComponent.class.getDeclaredField("pickupThrottle");
            fDelay.setAccessible(true);
            fThrottle.setAccessible(true);
            return true;
        } catch (Throwable t) { fDelay = null; return false; }
    }

    private void applyDrops(Ref<EntityStore> ref, Store<EntityStore> store) {
        boolean vac = vacuum && !sharedLocked(), dup = autoDupe, fast = fastPickup;
        if (!vac && !dup && !fast) { seenItems.clear(); thrown.clear(); dupeWasOn = false; return; }
        TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
        if (tc == null || !ensureDelayFields()) return;
        Vector3d me = new Vector3d(tc.getPosition());
        boolean firstPass = seenItems.isEmpty();

        // 1) first sight of every drop in the world: fresh? thrown?
        List<ItemStack> freshStacks = new ArrayList<>();
        List<Vector3d> freshWhere = new ArrayList<>();
        List<ItemComponent> nearComps = new ArrayList<>();
        List<Ref<EntityStore>> nearRefs = new ArrayList<>();
        double vr2 = (double) vacuumRadius * vacuumRadius;
        store.forEachChunk(ItemComponent.getComponentType(), (chunk, cb) -> {
            for (int i = 0; i < chunk.size(); i++) {
                ItemComponent ic = chunk.getComponent(i, ItemComponent.getComponentType());
                TransformComponent t = chunk.getComponent(i, TransformComponent.getComponentType());
                if (ic == null || t == null || ic.getItemStack() == null || ic.getItemStack().isEmpty()) continue;
                Ref<EntityStore> r = chunk.getReferenceTo(i);
                double d2 = t.getPosition().distanceSquared(me);
                if (seenItems.add(r)) {
                    float delay;
                    try { delay = fDelay.getFloat(ic); } catch (Throwable e) { delay = 0f; }
                    if (delay > ItemComponent.DEFAULT_PICKUP_DELAY + 0.05f) thrown.add(r);
                    // fresh drop near you, and not on the pass that just switched on
                    if (dup && !firstPass && delay > 0f && d2 <= 64.0 * 64.0) {
                        freshStacks.add(ic.getItemStack());
                        freshWhere.add(new Vector3d(t.getPosition()));
                    }
                }
                if (thrown.contains(r)) continue;
                if (fast && d2 <= 64.0 * 64.0) { nearComps.add(ic); }
                if (vac && d2 <= vr2 && d2 > 1.0) nearRefs.add(r);
            }
        });

        // 2) duplicate (before Instant Pickup takes the delay away)
        if (dup && dupeWasOn && !freshStacks.isEmpty()) duplicate(store, freshStacks, freshWhere, dupeMult);
        dupeWasOn = dup;

        // 3) Instant Pickup: no delay, no throttle between attempts
        for (ItemComponent ic : nearComps) {
            try {
                if (fDelay.getFloat(ic) > 0f) ic.setPickupDelay(0f);
                fThrottle.setFloat(ic, 0f);
            } catch (Throwable ignored) {}
        }

        // 4) Vacuum: drops in range come to your feet; normal pickup takes them
        //    (so a full inventory is respected)
        if (vac && ticks % 2 == 0) {
            Vector3d dest = new Vector3d(me).add(0, 0.3, 0);
            for (Ref<EntityStore> r : nearRefs) {
                if (!r.isValid()) continue;
                store.putComponent(r, Teleport.getComponentType(), new Teleport(dest, new Rotation3f()));
                pulled++;
            }
        }
        if (ticks % 30 == 0) {
            seenItems.removeIf(r -> !r.isValid());
            thrown.removeIf(r -> !r.isValid());
        }
    }

    // ---------------------------------------------------------------------
    //  Projectiles
    //
    //  Both kinds of projectile (ProjectileComponent and the newer
    //  StandardPhysicsProvider) record who fired them, and both reload their
    //  position from the TransformComponent and their velocity from the
    //  Velocity component at the start of every physics tick. Rewriting those
    //  steers a projectile; the game's own collision then makes the hit, so
    //  the weapon's real damage, effects and knockback apply.
    // ---------------------------------------------------------------------
    private UUID creatorOf(ArchetypeChunk<EntityStore> chunk, int i) {
        try {
            ProjectileComponent pc = chunk.getComponent(i, ProjectileComponent.getComponentType());
            if (pc != null) return pc.getCreatorUuid();
        } catch (Throwable ignored) {}
        try {
            StandardPhysicsProvider sp = chunk.getComponent(i, StandardPhysicsProvider.getComponentType());
            if (sp != null) return sp.getCreatorUuid();
        } catch (Throwable ignored) {}
        return null;
    }

    // Where to aim on a target: around its chest.
    private static Vector3d aimPoint(Store<EntityStore> store, Ref<EntityStore> t) {
        TransformComponent tt = store.getComponent(t, TransformComponent.getComponentType());
        if (tt == null) return null;
        return new Vector3d(tt.getPosition()).add(0, height(store, t) * 0.6, 0);
    }

    // The valid target closest to the projectile's line of flight, within the
    // cone and range.
    private Ref<EntityStore> pickTarget(Ref<EntityStore> self, Store<EntityStore> store, Vector3d me, Vector3d from, Vector3d dir) {
        double cosMax = Math.cos(Math.toRadians(projCone));
        Ref<EntityStore> best = null;
        double bestCos = cosMax;
        for (Target t : targets(self, store, me, projRange)) {
            Vector3d aim = aimPoint(store, t.ref());
            if (aim == null) continue;
            Vector3d to = new Vector3d(aim).sub(from);
            double len = to.length();
            if (len < 0.5) continue;
            double c = to.dot(dir) / len;
            if (c > bestCos) { bestCos = c; best = t.ref(); }
        }
        return best;
    }

    private void applyProjectiles(Ref<EntityStore> self, Store<EntityStore> store) {
        if (!(projAim || magicBullet) || sharedLocked() || owner == null) {
            projTargets.clear(); projPlaced.clear(); return;
        }
        TransformComponent tc = store.getComponent(self, TransformComponent.getComponentType());
        if (tc == null) return;
        Vector3d me = new Vector3d(tc.getPosition());

        List<Ref<EntityStore>> mine = new ArrayList<>();
        store.forEachChunk(Velocity.getComponentType(), (chunk, cb) -> {
            for (int i = 0; i < chunk.size(); i++)
                if (owner.equals(creatorOf(chunk, i))) mine.add(chunk.getReferenceTo(i));
        });

        for (Ref<EntityStore> p : mine) {
            if (!p.isValid()) continue;
            TransformComponent pt = store.getComponent(p, TransformComponent.getComponentType());
            Velocity v = store.getComponent(p, Velocity.getComponentType());
            if (pt == null || v == null) continue;
            Vector3d pos = new Vector3d(pt.getPosition());
            Vector3d vel = new Vector3d(v.getX(), v.getY(), v.getZ());
            double speed = vel.length();
            if (speed < 0.5) continue;      // landed or stuck: only projectiles in flight are touched

            Ref<EntityStore> target = projTargets.get(p);
            if (target == null || !target.isValid() || store.getArchetype(target).contains(DeathComponent.getComponentType())) {
                Vector3d dir = new Vector3d(vel).div(speed);
                target = pickTarget(self, store, me, pos, dir);
                if (target == null) continue;
                projTargets.put(p, target);
            }
            Vector3d aim = aimPoint(store, target);
            if (aim == null) continue;
            Vector3d to = new Vector3d(aim).sub(pos);
            double dist = to.length();
            if (dist < 0.05) continue;
            Vector3d dir = new Vector3d(to).div(dist);
            double fly = Math.max(speed, 20.0);

            // Magic bullet: once, put it just in front of the target, flying in.
            if (magicBullet && projPlaced.add(p)) {
                if (dist > 1.6) pt.teleportPosition(new Vector3d(aim).sub(new Vector3d(dir).mul(1.2)));
                v.set(dir.x * fly, dir.y * fly, dir.z * fly);
                bullets++;
                continue;
            }
            // Homing: point it at the target every tick (gravity included).
            if (projAim) {
                v.set(dir.x * Math.max(speed, 12.0), dir.y * Math.max(speed, 12.0), dir.z * Math.max(speed, 12.0));
                if (projPlaced.add(p)) homed++;
            }
        }
        if (ticks % 30 == 0) {
            projTargets.keySet().removeIf(r -> !r.isValid());
            projPlaced.removeIf(r -> !r.isValid());
        }
    }

    // Returns true when something was hit.
    private boolean combatSwing(Ref<EntityStore> ref, Store<EntityStore> store) {
        long before = hits + ghosted;
        TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
        if (tc == null) return false;
        Vector3d me = new Vector3d(tc.getPosition());
        double eyeH = ModelComponent.getEyeHeight(ref, store);
        HeadRotation hr = store.getComponent(ref, HeadRotation.getComponentType());
        Rotation3f rot = hr != null ? hr.getRotation() : tc.getRotation();
        Vector3d dir = Transform.getDirection(rot.pitch(), rot.yaw());
        Vector3d eye = new Vector3d(me).add(0, eyeH, 0);

        // Ghost hit: the nearest target out of arm's reach comes to you, right
        // in front of you, so the swing you just started lands on it.
        if (ghostHit) {
            Target best = null;
            double bd = Double.MAX_VALUE;
            for (Target t : targets(ref, store, me, ghostRange)) {
                double d = t.pos().distance(me);
                if (d > 2.2 && d < bd) { bd = d; best = t; }
            }
            if (best != null && best.ref().isValid()) {
                double fx = dir.x, fz = dir.z, l = Math.sqrt(fx * fx + fz * fz);
                if (l < 1e-3) { fx = 0; fz = -1; l = 1; }
                Vector3d dest = new Vector3d(me).add(fx / l * 1.6, 0, fz / l * 1.6);
                if (best.player()) store.putComponent(best.ref(), Teleport.getComponentType(), Teleport.createForPlayer(dest, new Rotation3f()));
                else store.putComponent(best.ref(), Teleport.getComponentType(), new Teleport(dest, new Rotation3f()));
                ghosted++;
            }
        }

        // Reach: whatever you are aiming at, up to reachRange away.
        if (swingReach) {
            Target best = null;
            double bestScore = Double.MAX_VALUE;
            for (Target t : targets(ref, store, me, reachRange + 2)) {
                Vector3d c = new Vector3d(t.pos()).add(0, 0.9, 0);
                Vector3d to = new Vector3d(c).sub(eye);
                double along = to.dot(dir);
                if (along <= 0 || along > reachRange) continue;
                double perp = new Vector3d(to).sub(new Vector3d(dir).mul(along)).length();
                if (perp > 1.2) continue;                                  // not under the crosshair
                double score = perp + along * 0.05;
                if (score < bestScore) { bestScore = score; best = t; }
            }
            if (best != null) { hit(ref, store, best.ref()); return true; }
        }

        // Killaura: the nearest targets in range, up to auraMax of them.
        if (swingAura) {
            List<Target> ts = targets(ref, store, me, aaRadius);
            ts.sort(Comparator.comparingDouble(t -> t.pos().distanceSquared(me)));
            for (int i = 0; i < ts.size() && i < auraMax; i++) hit(ref, store, ts.get(i).ref());
        }
        return hits + ghosted != before;
    }

    // ---------------------------------------------------------------------
    //  Timed auto attack: the nearest target in reach, at a fixed rate.
    // ---------------------------------------------------------------------
    private void autoAttack(Ref<EntityStore> ref, Store<EntityStore> store) {
        if (!autoAttack || sharedLocked()) return;
        long now = System.currentTimeMillis();
        if (now - lastHit < (long) (1000f / Math.max(0.2f, aaRate))) return;
        TransformComponent tc = store.getComponent(ref, TransformComponent.getComponentType());
        if (tc == null) return;
        Vector3d me = tc.getPosition();
        double eyeH = ModelComponent.getEyeHeight(ref, store);
        HeadRotation hr = store.getComponent(ref, HeadRotation.getComponentType());
        Rotation3f rot = hr != null ? hr.getRotation() : tc.getRotation();
        Vector3d dir = Transform.getDirection(rot.pitch(), rot.yaw());
        Target best = null;
        double bd = Double.MAX_VALUE;
        for (Target t : targets(ref, store, me, aaRadius)) {
            double dx = t.pos().x - me.x, dy = t.pos().y + 0.9 - (me.y + eyeH), dz = t.pos().z - me.z;
            double d2 = dx * dx + dy * dy + dz * dz;
            if (aaFrontOnly) {
                double len = Math.sqrt(d2);
                if (len > 0.001 && (dx * dir.x + dy * dir.y + dz * dir.z) / len < 0.5) continue;
            }
            if (d2 < bd) { bd = d2; best = t; }
        }
        if (best == null) return;
        lastHit = now;
        hit(ref, store, best.ref());
    }

    private static void teleport(Ref<EntityStore> ref, Store<EntityStore> store, Vector3d pos) {
        HeadRotation hr = store.getComponent(ref, HeadRotation.getComponentType());
        Rotation3f r = hr != null ? new Rotation3f(hr.getRotation()) : new Rotation3f();
        store.putComponent(ref, Teleport.getComponentType(), Teleport.createForPlayer(pos, r));
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static int repairAll(Ref<EntityStore> ref, Store<EntityStore> store, boolean onlyWorn) {
        int n = 0;
        ComponentType[] types = {
            InventoryComponent.Hotbar.getComponentType(), InventoryComponent.Armor.getComponentType(),
            InventoryComponent.Utility.getComponentType(), InventoryComponent.Tool.getComponentType(),
            InventoryComponent.Storage.getComponentType(), InventoryComponent.Backpack.getComponentType() };
        for (ComponentType t : types) {
            if (t == null) continue;
            Object c = store.getComponent(ref, t);
            if (!(c instanceof InventoryComponent inv) || inv.getInventory() == null) continue;
            ItemContainer ic = inv.getInventory();
            for (short s = 0; s < ic.getCapacity(); s++) {
                ItemStack st = ic.getItemStack(s);
                if (st == null || st.isEmpty() || st.isUnbreakable() || st.getMaxDurability() <= 0) continue;
                if (st.getDurability() >= st.getMaxDurability()) continue;
                if (ic.replaceItemStackInSlot(s, st, st.withDurability(st.getMaxDurability())).succeeded()) n++;
            }
        }
        return n;
    }

    private int killNear(Ref<EntityStore> ref, Store<EntityStore> store, double radius, boolean hostileOnly) {
        Vector3d me = store.getComponent(ref, TransformComponent.getComponentType()).getPosition();
        double r2 = radius * radius;
        List<Ref<EntityStore>> victims = new ArrayList<>();
        store.forEachChunk(NPCEntity.getComponentType(), (chunk, cb) -> {
            if (chunk.getArchetype().contains(DeathComponent.getComponentType())) return;
            for (int i = 0; i < chunk.size(); i++) {
                TransformComponent t = chunk.getComponent(i, TransformComponent.getComponentType());
                if (t == null || t.getPosition().distanceSquared(me) > r2) continue;
                if (hostileOnly) {
                    WorldSupport ws = chunk.getComponent(i, WorldSupport.getComponentType());
                    if (ws == null || ws.getDefaultPlayerAttitude() != Attitude.HOSTILE) continue;
                }
                victims.add(chunk.getReferenceTo(i));
            }
        });
        // Kill as the player so drops and kill credit behave like a normal kill.
        for (Ref<EntityStore> v : victims) {
            if (!v.isValid()) continue;
            DeathComponent.tryAddComponent(store, v, new Damage(new Damage.EntitySource(ref), DamageCause.PHYSICAL, 2.1474836E9F));
        }
        return victims.size();
    }

    // ---------------------------------------------------------------------
    //  Restore (called once from Core.stop on the net thread)
    // ---------------------------------------------------------------------
    void restore() {
        god = infHealth = infStamina = infMana = infOxygen = infAmmo = noFall = false;
        fly = noClip = infDurability = freezeMobs = freezeTime = oneHit = false;
        autoAttack = swingAura = swingReach = ghostHit = instantBreak = vacuum = autoDupe = fastPickup = false;
        projAim = magicBullet = false;
        freezeMobsEff = false;
        speed = jump = flySpeed = damageMult = 1f;
        World w = lastWorld;
        if (w == null || !w.isAlive()) return;
        CompletableFuture<Void> done = new CompletableFuture<>();
        try {
            w.execute(() -> {
                try {
                    Ref<EntityStore> ref = ownerRef();
                    Store<EntityStore> store = lastStore;
                    if (store != null && store.isInThread()) {
                        unfreezeAll(store);
                        if (weatherForced) {
                            WeatherResource wr = store.getResource(WeatherResource.getResourceType());
                            if (wr != null) wr.setForcedWeather(w.getWorldConfig().getForcedWeather());
                        }
                        if (ref != null && ref.getStore() == store) {
                            applyInvulnerable(ref, store);
                            applyMovement(ref, store);
                            applyNoClip(ref, store);
                        }
                    }
                } catch (Throwable ignored) {
                } finally { done.complete(null); }
            });
            done.get(3, TimeUnit.SECONDS);
        } catch (Throwable ignored) {}
    }
}
