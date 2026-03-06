/*
 * ============================================================
 *  MOSQUITO HALL SIMULATION — Fully Debugged & Refined v2.0
 *  Hall Size  : 1000 sq ft (modeled as 100x100 unit grid)
 *  Duration   : 24 Hours (1440 minutes, 86400 ticks)
 *
 *  FIXES IN THIS VERSION:
 *    1. Array slot reuse    — dead slots recycled, no overflow
 *    2. Queen assassination — 5% chance on corner swing
 *    3. Volumetric blood    — 2.5 ul/dunk, FULL at 5.0 ul (2 dunks)
 *    4. Baby distress       — whimper / crying / screaming thresholds
 *    5. Tactical U-turn     — missed mosquitoes reverse direction
 *
 *  Compile : gcc mosquito_sim.c -o mosquito_sim -lm
 *  Run     : ./mosquito_sim
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * SECTION 1: GLOBAL CONSTANTS
 * ============================================================ */

/* --- Hall Dimensions --- */
#define HALL_WIDTH          100.0f
#define HALL_HEIGHT         100.0f

/* --- Population cap (array size stays 1000, but slots are REUSED) --- */
#define MAX_MOSQUITOES      1000

/* --- Coil (center of hall) --- */
#define COIL_X              50.0f
#define COIL_Y              50.0f
#define COIL_RADIUS         15.0f
#define COIL_SPEED_PENALTY  0.50f   /* 50% speed cut inside coil zone        */

/* --- Key character coordinates --- */
#define QUEEN_X             0.0f    /* Queen is at the corner (0,0)          */
#define QUEEN_Y             0.0f
#define BABY_X              50.0f   /* Baby is at center (50,50)             */
#define BABY_Y              50.0f

/* --- Mosquito speeds --- */
#define SPEED_ADULT         5.0f
#define SPEED_BABY          2.0f

/* --- Spawn settings --- */
#define SPAWN_COUNT         10      /* New mosquitoes per wave               */
#define SPAWN_INTERVAL      5       /* Wave fires every 5 minutes            */

/* --- Timing --- */
#define TICKS_PER_MINUTE    60
#define MAX_AGE_TICKS       (30 * TICKS_PER_MINUTE)   /* Natural death: 30 min */
#define RECOVERY_TICKS      (5  * TICKS_PER_MINUTE)   /* Dizzy window: 5 min   */

/* --- Racket --- */
#define RACKET_RADIUS       2.0f
#define RACKET_HIT_CHANCE   75      /* 75% kill on contact                   */
#define RACKET_SWING_CHANCE 2       /* 2% chance per tick player swings      */
#define QUEEN_KILL_RADIUS   8.0f    /* Swing within this radius of (0,0) can */
                                    /* trigger queen assassination            */
#define QUEEN_KILL_CHANCE   5       /* 5% probability queen is killed        */

/* --- Recovery --- */
#define RECOVERY_CHANCE     50      /* 50% chance dizzy -> active            */

/* --- FIX 3: Volumetric blood (microliters) --- */
#define BLOOD_PER_DUNK      2.5f    /* Each successful bite = 2.5 ul         */
#define BLOOD_FULL_AT       5.0f    /* FULL after 5.0 ul (exactly 2 dunks)   */
#define FEED_RANGE          5.0f    /* Must be within 5 units of baby        */

/* --- Baby distress thresholds (active mosquitoes near baby) --- */
#define DISTRESS_WHIMPER_MIN   3    /* 3-5  mosquitoes near baby             */
#define DISTRESS_WHIMPER_MAX   5
#define DISTRESS_CRY_MIN       6    /* 6-10 mosquitoes near baby             */
#define DISTRESS_CRY_MAX      10
#define DISTRESS_SCREAM_MIN   11    /* >10  mosquitoes near baby             */

/* --- Report box width --- */
#define BOX_WIDTH           64


/* ============================================================
 * SECTION 2: ENUMS & STRUCTS
 * ============================================================ */

typedef enum {
    STATE_ACTIVE = 0,   /* Healthy, hunting for blood                       */
    STATE_DIZZY  = 1,   /* Coil-affected, slowed                            */
    STATE_DEAD   = 2,   /* Dead — slot available for reuse                  */
    STATE_FULL   = 3    /* Fed to capacity, retreating                      */
} MosquitoState;

typedef struct {
    int   id;               /* Unique ID (increments globally, never resets)*/
    float x;                /* X position in hall                           */
    float y;                /* Y position in hall                           */
    int   age;              /* Age in ticks                                 */
    float speed;            /* Current movement speed (units/tick)          */
    float velX;             /* FIX 5: Last movement X-component (for U-turn)*/
    float velY;             /* FIX 5: Last movement Y-component (for U-turn)*/
    int   state;            /* Current MosquitoState                        */
    int   recoveryTimer;    /* Ticks left in dizzy recovery window          */
    float bloodSucked;      /* Cumulative microliters sucked                */
    int   dunkCount;        /* Number of successful bites (dunks) so far    */
} Mosquito;

typedef struct {
    int   totalBorn;        /* Total mosquitoes ever spawned                */
    int   killedByRacket;   /* Killed by player's racket                    */
    int   diedNaturally;    /* Died of old age                              */
    int   becameDizzy;      /* Total STATE_ACTIVE -> STATE_DIZZY events     */
    int   recoveredCount;   /* Dizzy mosquitoes that recovered              */
    int   successfullyFed;  /* Mosquitoes that reached STATE_FULL           */
    int   failedToFeed;     /* Died without sucking any blood               */
    float totalBloodConsumed; /* FIX 3: Grand total microliters extracted   */
    int   totalDunks;       /* Total individual bite events across all mozzies */
} GameState;


/* ============================================================
 * SECTION 3: GLOBAL VARIABLES
 * ============================================================ */
Mosquito  mosquitoes[MAX_MOSQUITOES];  /* Fixed pool — slots are REUSED     */
GameState gameState;                   /* Simulation statistics             */
int       mosquitoCount    = 0;        /* Highest used index + 1            */
int       isQueenAlive     = 1;        /* FIX 2: Queen alive flag (1=alive) */


/* ============================================================
 * SECTION 4: HELPER FUNCTIONS
 * ============================================================ */

float randomFloat(float min, float max) {
    return min + ((float)rand() / (float)RAND_MAX) * (max - min);
}

float distanceBetween(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    return (float)sqrt((double)(dx * dx + dy * dy));
}

int countByState(int targetState) {
    int i, count = 0;
    for (i = 0; i < mosquitoCount; i++)
        if (mosquitoes[i].state == targetState) count++;
    return count;
}

int countAlive(void) {
    int i, count = 0;
    for (i = 0; i < mosquitoCount; i++)
        if (mosquitoes[i].state != STATE_DEAD) count++;
    return count;
}

/*
 * FIX 4 HELPER: countActiveNearBaby()
 * Counts STATE_ACTIVE mosquitoes within FEED_RANGE of the baby.
 * Used each minute to trigger distress messages.
 */
int countActiveNearBaby(void) {
    int i, count = 0;
    for (i = 0; i < mosquitoCount; i++) {
        if (mosquitoes[i].state == STATE_ACTIVE) {
            if (distanceBetween(mosquitoes[i].x, mosquitoes[i].y,
                                BABY_X, BABY_Y) <= FEED_RANGE)
                count++;
        }
    }
    return count;
}

/*
 * initMosquito()
 * Resets a slot (new or recycled dead slot) to a fresh mosquito.
 * velX/velY start pointing in a random direction.
 */
void initMosquito(Mosquito *m, int id, int isAdult) {
    float angle          = randomFloat(0.0f, 2.0f * 3.14159265f);
    m->id                = id;
    m->x                 = randomFloat(0.0f, HALL_WIDTH);
    m->y                 = randomFloat(0.0f, HALL_HEIGHT);
    m->age               = 0;
    m->speed             = isAdult ? SPEED_ADULT : SPEED_BABY;
    m->velX              = (float)cos(angle);   /* unit vector component    */
    m->velY              = (float)sin(angle);
    m->state             = STATE_ACTIVE;
    m->recoveryTimer     = 0;
    m->bloodSucked       = 0.0f;
    m->dunkCount         = 0;
}


/* ============================================================
 * SECTION 5: CORE SIMULATION FUNCTIONS
 * ============================================================ */

/*
 * FIX 1 + FIX 2: spawnMosquitoes()
 * ---------------------------------
 * Every 5 minutes, attempts to add SPAWN_COUNT mosquitoes.
 * Instead of always appending, it first SEARCHES FOR A DEAD SLOT.
 * If the pool isn't full yet, it still appends to extend the array.
 * If isQueenAlive == 0, spawning is completely halted.
 */
void spawnMosquitoes(int minute) {
    int i, spawned, isAdult, slotIndex;

    /* FIX 2: Queen is dead — no more reproduction */
    if (!isQueenAlive) return;

    /* Only spawn on 5-minute boundaries, skip minute 0 */
    if (minute <= 0 || minute % SPAWN_INTERVAL != 0) return;

    spawned = 0;

    for (i = 0; i < SPAWN_COUNT; i++) {

        slotIndex = -1;   /* -1 means "no slot found yet" */

        /* --- FIX 1: Search for a reusable dead slot first --- */
        {
            int j;
            for (j = 0; j < mosquitoCount; j++) {
                if (mosquitoes[j].state == STATE_DEAD) {
                    slotIndex = j;
                    break;   /* Found one — stop searching */
                }
            }
        }

        /* If no dead slot found, try to extend the array */
        if (slotIndex == -1) {
            if (mosquitoCount < MAX_MOSQUITOES) {
                slotIndex = mosquitoCount;
                mosquitoCount++;
            } else {
                /* Array truly full with all-alive mosquitoes — skip */
                break;
            }
        }

        /* Alternate adult/baby: even wave-index = adult */
        isAdult = (i % 2 == 0) ? 1 : 0;
        initMosquito(&mosquitoes[slotIndex], gameState.totalBorn, isAdult);
        gameState.totalBorn++;
        spawned++;
    }
}

/*
 * updateMovement()
 * ----------------
 * Moves every living mosquito one step in a random direction.
 * Stores the direction vector in velX/velY for the U-turn mechanic.
 * Applies coil effects when inside COIL_RADIUS.
 */
void updateMovement(void) {
    int i;
    float angle, dx, dy, newX, newY, distToCoil;
    Mosquito *m;

    for (i = 0; i < mosquitoCount; i++) {
        m = &mosquitoes[i];

        if (m->state == STATE_DEAD) continue;

        /* Random direction, store as unit vector for potential U-turn */
        angle   = randomFloat(0.0f, 2.0f * 3.14159265f);
        dx      = (float)cos((double)angle);
        dy      = (float)sin((double)angle);
        m->velX = dx;
        m->velY = dy;

        /* Move by (speed * direction) */
        newX = m->x + m->speed * dx;
        newY = m->y + m->speed * dy;

        /* Clamp to hall walls */
        if (newX < 0.0f)        newX = 0.0f;
        if (newX > HALL_WIDTH)  newX = HALL_WIDTH;
        if (newY < 0.0f)        newY = 0.0f;
        if (newY > HALL_HEIGHT) newY = HALL_HEIGHT;

        m->x = newX;
        m->y = newY;

        /* Coil proximity check */
        distToCoil = distanceBetween(m->x, m->y, COIL_X, COIL_Y);

        if (distToCoil <= COIL_RADIUS) {
            m->speed = m->speed * (1.0f - COIL_SPEED_PENALTY);
            if (m->speed < 0.1f) m->speed = 0.1f;

            if (m->state == STATE_ACTIVE) {
                m->state         = STATE_DIZZY;
                m->recoveryTimer = RECOVERY_TICKS;
                gameState.becameDizzy++;
            }
        } else {
            /* Restore normal speed once outside coil zone */
            if (m->state == STATE_ACTIVE || m->state == STATE_FULL)
                m->speed = SPEED_ADULT;
        }
    }
}

/*
 * FIX 2 + FIX 5: handleRacket()
 * --------------------------------
 * Swings the racket at (swingX, swingY).
 * KILL  path (75%): mosquito dies → stats updated.
 * MISS  path (25%): FIX 5 — mosquito executes a U-turn by negating
 *                   its stored velocity vector (velX, velY).
 * QUEEN path: if the swing is within QUEEN_KILL_RADIUS of (0,0)
 *             and rand() % 100 < 5, the queen is assassinated and
 *             future spawning halts permanently.
 */
void handleRacket(float swingX, float swingY) {
    int i, roll;
    Mosquito *m;
    float newX, newY;

    /* --- FIX 2: Queen assassination check ---
     * Only check if queen is still alive and swing is near corner   */
    if (isQueenAlive) {
        float distToQueen = distanceBetween(swingX, swingY, QUEEN_X, QUEEN_Y);
        if (distToQueen <= QUEEN_KILL_RADIUS) {
            if ((rand() % 100) < QUEEN_KILL_CHANCE) {
                isQueenAlive = 0;
                printf("\n  [SYSTEM] >>> The Queen has been killed!"
                       " Population growth stopped. <<<\n\n");
            }
        }
    }

    /* --- Process each living mosquito --- */
    for (i = 0; i < mosquitoCount; i++) {
        m = &mosquitoes[i];

        if (m->state == STATE_DEAD) continue;

        if (distanceBetween(m->x, m->y, swingX, swingY) <= RACKET_RADIUS) {

            roll = rand() % 100;

            if (roll < RACKET_HIT_CHANCE) {
                /* ── HIT (75%): Kill the mosquito ── */
                m->state = STATE_DEAD;
                gameState.killedByRacket++;
                if (m->bloodSucked <= 0.0f)
                    gameState.failedToFeed++;

            } else {
                /* ── MISS (25%): FIX 5 — Execute U-turn ──
                 * Negate the stored velocity direction so the
                 * mosquito flies directly away from the swing  */
                m->velX = -(m->velX);
                m->velY = -(m->velY);

                /* Apply one U-turn step immediately */
                newX = m->x + m->speed * m->velX;
                newY = m->y + m->speed * m->velY;

                /* Clamp escape move to hall walls */
                if (newX < 0.0f)        newX = 0.0f;
                if (newX > HALL_WIDTH)  newX = HALL_WIDTH;
                if (newY < 0.0f)        newY = 0.0f;
                if (newY > HALL_HEIGHT) newY = HALL_HEIGHT;

                m->x = newX;
                m->y = newY;
            }
        }
    }
}

/*
 * handleAging()
 * -------------
 * Per-tick duties:
 *   1. Increment age; kill at MAX_AGE_TICKS.
 *   2. FIX 3: ACTIVE mosquitoes near baby perform a dunk (2.5 ul).
 *             FULL threshold is 5.0 ul (2 dunks).
 *   3. Dizzy recovery timer countdown with 50% recovery chance.
 */
void handleAging(void) {
    int i, recoveryRoll;
    float distToBaby;
    Mosquito *m;

    for (i = 0; i < mosquitoCount; i++) {
        m = &mosquitoes[i];

        if (m->state == STATE_DEAD) continue;

        /* --- 1. Age increment & natural death --- */
        m->age++;
        if (m->age >= MAX_AGE_TICKS) {
            m->state = STATE_DEAD;
            gameState.diedNaturally++;
            if (m->bloodSucked <= 0.0f)
                gameState.failedToFeed++;
            continue;
        }

        /* --- FIX 3: Feeding / Dunk logic ---
         * ACTIVE mosquito within FEED_RANGE of baby performs a bite.
         * Each bite (dunk) extracts BLOOD_PER_DUNK (2.5 ul).
         * Reaching BLOOD_FULL_AT (5.0 ul) = 2 dunks = STATE_FULL.  */
        if (m->state == STATE_ACTIVE) {
            distToBaby = distanceBetween(m->x, m->y, BABY_X, BABY_Y);

            if (distToBaby <= FEED_RANGE) {
                /* Perform one dunk this tick */
                m->bloodSucked              += BLOOD_PER_DUNK;
                m->dunkCount++;
                gameState.totalBloodConsumed += BLOOD_PER_DUNK;
                gameState.totalDunks++;

                /* Check if mosquito is now full (>= 5.0 ul) */
                if (m->bloodSucked >= BLOOD_FULL_AT) {
                    m->state = STATE_FULL;
                    gameState.successfullyFed++;
                }
            }
        }

        /* --- 3. Dizzy recovery timer --- */
        if (m->state == STATE_DIZZY && m->recoveryTimer > 0) {
            m->recoveryTimer--;

            if (m->recoveryTimer == 0) {
                recoveryRoll = rand() % 2;   /* 0 = recover, 1 = stay dizzy */

                if (recoveryRoll == 0) {
                    m->state = STATE_ACTIVE;
                    m->speed = SPEED_ADULT;
                    gameState.recoveredCount++;
                } else {
                    /* Failed — reset timer for another 5-min window */
                    m->recoveryTimer = RECOVERY_TICKS;
                }
            }
        }
    }
}


/* ============================================================
 * SECTION 6: BABY DISTRESS CHECKER  (FIX 4)
 * ============================================================ */

/*
 * checkBabyDistress()
 * -------------------
 * Called once per minute inside the main loop.
 * Counts ACTIVE mosquitoes within FEED_RANGE of the baby and
 * prints the appropriate distress message.
 * Returns the count so the caller can use it if needed.
 */
int checkBabyDistress(int minute) {
    int nearCount = countActiveNearBaby();

    if (nearCount >= DISTRESS_SCREAM_MIN) {
        printf("  [%02d:%02d] *** Baby is SCREAMING LOUDLY! "
               "(%d mosquitoes attacking!) ***\n",
               minute / 60, minute % 60, nearCount);

    } else if (nearCount >= DISTRESS_CRY_MIN && nearCount <= DISTRESS_CRY_MAX) {
        printf("  [%02d:%02d]  ** Baby is Crying! "
               "(%d mosquitoes near baby)\n",
               minute / 60, minute % 60, nearCount);

    } else if (nearCount >= DISTRESS_WHIMPER_MIN && nearCount <= DISTRESS_WHIMPER_MAX) {
        printf("  [%02d:%02d]   * Baby is whimpering. "
               "(%d mosquitoes nearby)\n",
               minute / 60, minute % 60, nearCount);
    }

    return nearCount;
}


/* ============================================================
 * SECTION 7: REPORT PRINTING HELPERS
 * ============================================================ */

void printSeparator(char c) {
    int i;
    printf("+");
    for (i = 0; i < BOX_WIDTH - 2; i++) printf("%c", c);
    printf("+\n");
}

void printBlank(void) {
    printf("|%*s|\n", BOX_WIDTH - 2, "");
}

void printCenter(const char *text) {
    int len   = (int)strlen(text);
    int inner = BOX_WIDTH - 2;
    int left  = (inner - len) / 2;
    int right = inner - len - left;
    int i;
    printf("|");
    for (i = 0; i < left;  i++) printf(" ");
    printf("%s", text);
    for (i = 0; i < right; i++) printf(" ");
    printf("|\n");
}

void printRowInt(const char *label, int value) {
    printf("| %-42s %17d |\n", label, value);
}

void printRowFloat(const char *label, float value) {
    printf("| %-42s %17.2f |\n", label, value);
}

void printRowPct(const char *label, float pct) {
    printf("| %-42s %16.1f%% |\n", label, pct);
}

void printSnapshot(int minute) {
    printf("  [%02d:%02d]  Alive:%-5d  Active:%-5d  Dizzy:%-4d"
           "  Full:%-4d  Dead:%-5d  Born:%-5d  Racket:%d\n",
           minute / 60, minute % 60,
           countAlive(),
           countByState(STATE_ACTIVE),
           countByState(STATE_DIZZY),
           countByState(STATE_FULL),
           countByState(STATE_DEAD),
           gameState.totalBorn,
           gameState.killedByRacket);
}


/* ============================================================
 * SECTION 8: FINAL REPORT
 * ============================================================ */

void printFinalReport(void) {
    int   totalDead    = gameState.killedByRacket + gameState.diedNaturally;
    int   stillAlive   = countAlive();
    float survivalRate = (gameState.totalBorn > 0)
                         ? (100.0f * stillAlive / gameState.totalBorn) : 0.0f;
    float racketEff    = (gameState.totalBorn > 0)
                         ? (100.0f * gameState.killedByRacket / gameState.totalBorn) : 0.0f;
    float feedRate     = (gameState.totalBorn > 0)
                         ? (100.0f * gameState.successfullyFed / gameState.totalBorn) : 0.0f;
    float recovRate    = (gameState.becameDizzy > 0)
                         ? (100.0f * gameState.recoveredCount / gameState.becameDizzy) : 0.0f;

    printf("\n\n");

    /* ── TOP BANNER ─────────────────────────────────── */
    printSeparator('=');
    printBlank();
    printCenter("MOSQUITO HALL SIMULATION v2.0 — FINAL REPORT");
    printCenter("24 Hours  |  1000 sq ft  |  86400 Ticks");
    printCenter(isQueenAlive ? "Queen Status: ALIVE" : "Queen Status: ASSASSINATED");
    printBlank();
    printSeparator('=');

    /* ── SECTION 1: POPULATION ───────────────────── */
    printBlank();
    printCenter("[ 1 ]  POPULATION OVERVIEW");
    printBlank();
    printRowInt  ("  Total Mosquitoes Ever Born",         gameState.totalBorn);
    printRowInt  ("  Mosquitoes Still Alive at End",      stillAlive);
    printRowInt  ("    |-- Active  (hunting)",            countByState(STATE_ACTIVE));
    printRowInt  ("    |-- Dizzy   (coil-affected)",      countByState(STATE_DIZZY));
    printRowInt  ("    |-- Full    (fed & retreating)",   countByState(STATE_FULL));
    printRowInt  ("  Total Mosquitoes Dead",              totalDead);
    printRowPct  ("  Survival Rate  (alive / born)",      survivalRate);
    printBlank();
    printSeparator('-');

    /* ── SECTION 2: CAUSE OF DEATH ───────────────── */
    printBlank();
    printCenter("[ 2 ]  CAUSE OF DEATH BREAKDOWN");
    printBlank();
    printRowInt  ("  Killed by Racket Swing",             gameState.killedByRacket);
    printRowInt  ("  Died of Old Age (natural)",          gameState.diedNaturally);
    printRowInt  ("  Total Deaths",                       totalDead);
    printRowPct  ("  Racket Kill Efficiency  (%  born)",  racketEff);
    printBlank();
    printSeparator('-');

    /* ── SECTION 3: COIL PERFORMANCE ────────────── */
    printBlank();
    printCenter("[ 3 ]  MOSQUITO COIL PERFORMANCE");
    printBlank();
    printRowInt  ("  Times a Mosquito Became Dizzy",      gameState.becameDizzy);
    printRowInt  ("  Recovered  (Dizzy -> Active)",       gameState.recoveredCount);
    printRowInt  ("  Still Dizzy at Sim End",             countByState(STATE_DIZZY));
    printRowPct  ("  Dizzy Recovery Rate",                recovRate);
    printBlank();
    printSeparator('-');

    /* ── SECTION 4: FEEDING (FIX 3) ─────────────── */
    printBlank();
    printCenter("[ 4 ]  FEEDING & BLOOD STATISTICS");
    printBlank();
    printRowInt  ("  Successfully Fed  (reached FULL)",   gameState.successfullyFed);
    printRowInt  ("  Failed to Feed    (died unfed)",     gameState.failedToFeed);
    printRowInt  ("  Total Bite Events (Dunks)",          gameState.totalDunks);
    printRowFloat("  Blood Per Dunk    (microliters)",    BLOOD_PER_DUNK);
    printRowFloat("  Full Threshold    (microliters)",    BLOOD_FULL_AT);
    printRowFloat("  Total Blood Consumed  (ul)",         gameState.totalBloodConsumed);
    printRowPct  ("  Feed Success Rate",                  feedRate);
    printBlank();
    printSeparator('-');

    /* ── SECTION 5: QUEEN STATUS (FIX 2) ────────── */
    printBlank();
    printCenter("[ 5 ]  QUEEN & SPAWN CONTROL");
    printBlank();
    printRowInt  ("  Queen Still Alive?  (1=Yes 0=No)",   isQueenAlive);
    printRowInt  ("  Spawn Waves Fired",                  gameState.totalBorn / SPAWN_COUNT);
    printBlank();
    printSeparator('-');

    /* ── SECTION 6: SIM PARAMETERS ──────────────── */
    printBlank();
    printCenter("[ 6 ]  SIMULATION PARAMETERS");
    printBlank();
    printRowInt  ("  Total Minutes Simulated",            1440);
    printRowInt  ("  Total Ticks  (min x 60)",            86400);
    printRowInt  ("  Spawn Wave Size",                    SPAWN_COUNT);
    printRowInt  ("  Spawn Interval       (minutes)",     SPAWN_INTERVAL);
    printRowInt  ("  Max Lifespan         (minutes)",     30);
    printRowInt  ("  Coil Radius          (units)",       (int)COIL_RADIUS);
    printRowInt  ("  Racket Radius        (units)",       (int)RACKET_RADIUS);
    printRowInt  ("  Racket Hit Chance    (%)",           RACKET_HIT_CHANCE);
    printRowInt  ("  Queen Kill Radius    (units)",       (int)QUEEN_KILL_RADIUS);
    printRowInt  ("  Queen Kill Chance    (%)",           QUEEN_KILL_CHANCE);
    printRowInt  ("  Dizzy Recovery Window (minutes)",    5);
    printRowInt  ("  Recovery Chance      (%)",           RECOVERY_CHANCE);
    printRowInt  ("  Array Slots (reused) (MAX_MOSQ)",    MAX_MOSQUITOES);
    printBlank();

    /* ── FOOTER ──────────────────────────────────── */
    printSeparator('=');
    printBlank();
    printCenter("Simulation complete. The hall is (somewhat) safe.");
    printBlank();
    printSeparator('=');
    printf("\n");
}


/* ============================================================
 * SECTION 9: MAIN — 24-HOUR SIMULATION LOOP
 * ============================================================ */

int main(void) {
    int   minute, tick;
    float swingX, swingY;

    /* Seed RNG */
    srand((unsigned int)time(NULL));

    /* Zero-initialise all globals */
    memset(mosquitoes, 0, sizeof(mosquitoes));
    memset(&gameState, 0, sizeof(GameState));
    mosquitoCount = 0;
    isQueenAlive  = 1;

    /* Opening banner */
    printf("\n");
    printf("+==============================================================+\n");
    printf("|      MOSQUITO HALL SIMULATION v2.0  —  STARTING             |\n");
    printf("|      Hall: 1000 sq ft  |  Duration: 24 Hours                |\n");
    printf("|      Array: Slot-reuse ON  |  Queen: ALIVE                  |\n");
    printf("+==============================================================+\n\n");
    printf("  [HH:MM]  Status snapshots printed every hour.\n");
    printf("  Distress alerts printed whenever baby is threatened.\n\n");
    printf("  %s\n\n",
           "--------------------------------------------------------------");

    /* ================================================================
     * OUTER LOOP — 1440 minutes (one full simulated day)
     * ================================================================ */
    for (minute = 0; minute < 1440; minute++) {

        /* Spawn wave check (every 5 min, halts if queen is dead) */
        spawnMosquitoes(minute);

        /* =============================================================
         * INNER LOOP — 60 ticks per minute
         * ============================================================= */
        for (tick = 0; tick < TICKS_PER_MINUTE; tick++) {

            /* 1. Move all mosquitoes; apply coil effects */
            updateMovement();

            /* 2. Age, feed (dunk logic), dizzy recovery */
            handleAging();

            /* 3. Random racket swing (~2% chance per tick) */
            if ((rand() % 100) < RACKET_SWING_CHANCE) {
                swingX = randomFloat(0.0f, HALL_WIDTH);
                swingY = randomFloat(0.0f, HALL_HEIGHT);
                handleRacket(swingX, swingY);
            }
        }

        /* FIX 4: Baby distress check — once per minute */
        checkBabyDistress(minute);

        /* Hourly snapshot line */
        if ((minute + 1) % 60 == 0)
            printSnapshot(minute + 1);
    }

    /* Final formatted report */
    printFinalReport();

    return 0;
}
