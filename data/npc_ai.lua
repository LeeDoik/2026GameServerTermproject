-- Aetheria NPC AI 스크립트 (PDF 기본 스펙 기반)
--
-- C++가 글로벌 상수 (TYPE_PEACE/TYPE_AGRO, MOVE_FIXED/MOVE_ROAMING,
-- AGRO_RANGE, ROAM_RANGE)를 미리 세팅한 뒤 OnTick(ctx)를 호출.
-- 반환: dx, dy, new_target_id  (3개 값을 multi-return)
--
-- ctx 필드:
--   id, npc_type, move_mode, x, y, spawn_x, spawn_y,
--   target_id, target_x, target_y,
--   nearest_id, nearest_x, nearest_y, nearest_dist

print("[lua] npc_ai.lua loaded (Lua " .. _VERSION .. " — Boss AI included)")

-- [perf] 워커별 VM마다 C++가 주입한 고유 시드 사용. 모든 워커가 같은 로밍 시퀀스를
-- 도는 것을 방지(없으면 os.time fallback — 단일 VM/구버전 호환).
math.randomseed(WORKER_SEED or os.time())

local function clamp_step(diff)
    if diff > 0 then return 1
    elseif diff < 0 then return -1
    else return 0 end
end

-- 한 축씩 1칸 (대각 X). 더 먼 축 우선 좁힘
local function step_toward(x1, y1, x2, y2)
    local ddx = math.abs(x2 - x1)
    local ddy = math.abs(y2 - y1)
    if ddx >= ddy and x2 ~= x1 then
        return clamp_step(x2 - x1), 0
    elseif y2 ~= y1 then
        return 0, clamp_step(y2 - y1)
    end
    return 0, 0
end

-- spawn 중심 ROAM_RANGE 박스(Chebyshev)를 벗어나는 축의 이동을 0으로 막음
local function clamp_to_spawn_box(ctx, dx, dy)
    if math.abs(ctx.x + dx - ctx.spawn_x) > ROAM_RANGE then dx = 0 end
    if math.abs(ctx.y + dy - ctx.spawn_y) > ROAM_RANGE then dy = 0 end
    return dx, dy
end

-- 현재 NPC가 이미 spawn 영역 밖에 있는지 (Chebyshev > ROAM_RANGE)
local function is_outside_spawn_box(ctx)
    return math.abs(ctx.x - ctx.spawn_x) > ROAM_RANGE
        or math.abs(ctx.y - ctx.spawn_y) > ROAM_RANGE
end

-- Roaming: 4방향 1칸 + 50% 정지. 스폰 중심 ±ROAM_RANGE 클램프
local function roaming_step(ctx)
    local r = math.random(0, 7)
    local dx, dy = 0, 0
    if     r == 0 then dx =  1
    elseif r == 1 then dx = -1
    elseif r == 2 then dy =  1
    elseif r == 3 then dy = -1
    end
    if math.abs(ctx.x + dx - ctx.spawn_x) > ROAM_RANGE then dx = 0 end
    if math.abs(ctx.y + dy - ctx.spawn_y) > ROAM_RANGE then dy = 0 end
    return dx, dy
end

-- 다음 step (x+dx, y+dy)이 target 좌표와 겹치는지 — 겹치면 NPC가 플레이어 타일에 올라타게 됨
local function would_step_onto(ctx, dx, dy, tx, ty)
    return (ctx.x + dx) == tx and (ctx.y + dy) == ty
end

function OnTick(ctx)
    -- 1) 이미 추적 중 (PDF: "처음 인식한 공격 대상을 계속 공격")
    --    target이 시야 안에 있으면 그대로 추적. 사라졌으면 해제.
    if ctx.target_id ~= -1 then
        if ctx.target_x == -1 then
            -- 추적 대상이 시야 밖으로 완전히 이탈 → 해제 + Roaming/Fixed로 복귀
            return 0, 0, -1
        end
        -- 자기 영역(spawn ± ROAM_RANGE) 밖이면 target 해제하고 복귀
        if is_outside_spawn_box(ctx) then
            return 0, 0, -1
        end
        local dx, dy = step_toward(ctx.x, ctx.y, ctx.target_x, ctx.target_y)
        -- 다음 위치가 target 위치와 겹치면 정지 (target 옆 카디널 인접 칸에 머무름)
        -- → 공격 핸들러의 인접 4타일 판정에 NPC가 정확히 잡힘
        if would_step_onto(ctx, dx, dy, ctx.target_x, ctx.target_y) then
            return 0, 0, ctx.target_id
        end
        dx, dy = clamp_to_spawn_box(ctx, dx, dy)
        return dx, dy, ctx.target_id
    end

    -- 2) Agro 타입이고 가장 가까운 player가 11x11(±AGRO_RANGE) 안에 있으면 추적 시작
    if ctx.npc_type == TYPE_AGRO
       and ctx.nearest_id ~= -1
       and ctx.nearest_dist >= 0
       and ctx.nearest_dist <= AGRO_RANGE then
        -- 안전장치: 트리거 시점에 이미 영역 밖이면 무시 (사실상 발생 X)
        if is_outside_spawn_box(ctx) then
            return 0, 0, -1
        end
        local dx, dy = step_toward(ctx.x, ctx.y, ctx.nearest_x, ctx.nearest_y)
        if would_step_onto(ctx, dx, dy, ctx.nearest_x, ctx.nearest_y) then
            return 0, 0, ctx.nearest_id
        end
        dx, dy = clamp_to_spawn_box(ctx, dx, dy)
        return dx, dy, ctx.nearest_id
    end

    -- 3) 비전투: 이동 모드별 분기
    if ctx.move_mode == MOVE_ROAMING then
        local dx, dy = roaming_step(ctx)
        return dx, dy, -1
    end

    -- Fixed (또는 Peace + 대기): 가만히
    return 0, 0, -1
end

-- ============================================================
-- Stage 7.2: 보스 AI
-- OnBossTick(ctx) → dx, dy, target_id, chat_id, do_boss_aoe
--   chat_id: 0=없음, 1=도발1, 2=도발2, 3=분노선언, 4=포효
--   do_boss_aoe: 0=없음, 1=이번 틱 AoE 발동
-- ============================================================

local BOSS_TAUNT = {
    "You dare enter my domain?!",
    "I will crush you!",
    "None shall pass!",
    "Your courage is admirable... and foolish.",
}
local BOSS_ENRAGE = "ENRAGED!! Witness my true power!!"
local BOSS_ROAR   = "ROARRR!!"

local function boss_step_toward(ctx)
    if ctx.target_x == -1 then return 0, 0 end
    local dx, dy = step_toward(ctx.x, ctx.y, ctx.target_x, ctx.target_y)
    if would_step_onto(ctx, dx, dy, ctx.target_x, ctx.target_y) then
        return 0, 0
    end
    return dx, dy
end

function OnBossTick(ctx)
    local tick       = ctx.boss_tick_count
    local hp_pct     = (ctx.max_hp > 0) and (ctx.hp / ctx.max_hp) or 0
    local phase2     = (hp_pct <= 0.5)

    local do_aoe  = 0
    local chat_id = 0

    -- AoE 발동 주기: BOSS_AOE_INTERVAL 틱마다 1회 (C++에서 BOSS_AOE_INTERVAL_TICKS 노출)
    if ctx.target_id ~= -1 and tick > 0 and (tick % BOSS_AOE_INTERVAL) == 0 then
        do_aoe = 1
    end

    -- 분노 2단계 진입 선언 (체력이 딱 50% 이하가 됐을 때, tick 1회만)
    if phase2 then
        if tick > 0 and (tick % BOSS_CHAT_INTERVAL) == 0 then
            -- 분노 채팅 교대로 출력
            local idx = math.floor(tick / BOSS_CHAT_INTERVAL) % 4
            if idx == 0 then chat_id = 4   -- ROAR
            elseif idx == 1 then chat_id = 1
            elseif idx == 2 then chat_id = 2
            else chat_id = 3 end
        end
        -- 분노 2단계 진입 첫 틱 선언
        if tick == 0 then chat_id = 3 end
    else
        -- 1단계: 전투 시작 시 도발 (target 첫 설정 틱 = target_id 설정되고 tick==1일 때)
        if ctx.target_id ~= -1 and tick % 20 == 1 then
            chat_id = (tick % 4) + 1
        end
    end

    -- 이동: 항상 현재 target 또는 가장 가까운 플레이어 추적
    if ctx.target_id ~= -1 then
        if ctx.target_x == -1 then
            return 0, 0, -1, 0, 0
        end
        local dx, dy = boss_step_toward(ctx)
        return dx, dy, ctx.target_id, chat_id, do_aoe
    end

    -- Agro 트리거: 보스는 더 넓은 범위(BOSS_AGRO_RANGE)
    if ctx.nearest_id ~= -1 and ctx.nearest_dist >= 0
       and ctx.nearest_dist <= BOSS_AGRO_RANGE then
        local dx, dy = step_toward(ctx.x, ctx.y, ctx.nearest_x, ctx.nearest_y)
        if would_step_onto(ctx, dx, dy, ctx.nearest_x, ctx.nearest_y) then
            dx, dy = 0, 0
        end
        -- 보스는 고정 위치지만 주변 이동 허용 (spawn ±BOSS_AGRO_RANGE)
        if math.abs(ctx.x + dx - ctx.spawn_x) > BOSS_AGRO_RANGE then dx = 0 end
        if math.abs(ctx.y + dy - ctx.spawn_y) > BOSS_AGRO_RANGE then dy = 0 end
        chat_id = 1  -- 첫 타겟 잡을 때 도발
        return dx, dy, ctx.nearest_id, chat_id, do_aoe
    end

    -- 비전투: 스폰 위치로 복귀
    local dx, dy = step_toward(ctx.x, ctx.y, ctx.spawn_x, ctx.spawn_y)
    if ctx.x == ctx.spawn_x and ctx.y == ctx.spawn_y then dx, dy = 0, 0 end
    return dx, dy, -1, 0, 0
end
