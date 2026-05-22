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

print("[lua] npc_ai.lua loaded (Lua " .. _VERSION .. ")")

math.randomseed(os.time())

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
