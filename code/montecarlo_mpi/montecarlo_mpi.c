#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "engine.h"
#include "random.h"
#include "joseki/base.h"
#include "move.h"
#include "playout/moggy.h"
#include "playout/light.h"
#include "montecarlo_mpi/internal.h"
#include "montecarlo_mpi/montecarlo_mpi.h"
#include "playout.h"
#include "timeinfo.h"
#include <omp.h>
#include <math.h>
#include <sys/time.h>
#include "mpi.h"

extern int omp_thread_count;


/* This is simple monte-carlo engine. It plays MC_GAMES random games from the
 * current board and records win/loss ratio for each first move. The move with
 * the biggest number of winning games gets played. */
/* Note that while the library is based on New Zealand rules, this engine
 * returns moves according to Chinese rules. Thus, it does not return suicide
 * moves. It of course respects positional superko too. */

/* Pass me arguments like a=b,c=d,...
 * Supported arguments:
 * debug[=DEBUG_LEVEL]		1 is the default; more means more debugging prints
 * games=MC_GAMES		number of random games to play
 * gamelen=MC_GAMELEN		maximal length of played random game
 * playout={light,moggy}[:playout_params]
 */


#define MC_GAMES	40000
#define MC_GAMELEN	400


/* FIXME: Cutoff rule for simulations. Currently we are so fast that this
 * simply does not matter; even 100000 simulations are fast enough to
 * play 5 minutes S.D. on 19x19 and anything more sounds too ridiculous
 * already. */
/* FIXME: We cannot handle seki. Any good ideas are welcome. A possibility is
 * to consider 'pass' among the moves, but this seems tricky. */


void
board_stats_print_mpi(struct board *board, struct move_stat *moves, FILE *f)
{
	fprintf(f, "\n       ");
	int x, y;
	char asdf[] = "ABCDEFGHJKLMNOPQRSTUVWXYZ";
	for (x = 1; x < board_size(board) - 1; x++)
		fprintf(f, "%c    ", asdf[x - 1]);
	fprintf(f, "\n   +-");
	for (x = 1; x < board_size(board) - 1; x++)
		fprintf(f, "-----");
	fprintf(f, "+\n");
	for (y = board_size(board) - 2; y >= 1; y--) {
		fprintf(f, "%2d | ", y);
		for (x = 1; x < board_size(board) - 1; x++)
			if (moves[y * board_size(board) + x].games)
				fprintf(f, "%0.2f ", (floating_t) moves[y * board_size(board) + x].wins / moves[y * board_size(board) + x].games);
			else
				fprintf(f, "---- ");
		fprintf(f, "| ");
		for (x = 1; x < board_size(board) - 1; x++)
			fprintf(f, "%4d ", moves[y * board_size(board) + x].games);
		fprintf(f, "|\n");
	}
	fprintf(f, "   +-");
	for (x = 1; x < board_size(board) - 1; x++)
		fprintf(f, "-----");
	fprintf(f, "+\n");
}


static coord_t *
montecarlo_mpi_genmove(struct engine *e, struct board *b, struct time_info *ti, enum stone color, bool pass_all_alive)
{
	struct montecarlo_mpi *mc = e->data;
	if (ti->dim == TD_WALLTIME) {
		fprintf(stderr, "Warning: TD_WALLTIME time mode not supported, resetting to defaults.\n");
		ti->period = TT_NULL;
	}
	if (ti->period == TT_NULL) {
		ti->period = TT_MOVE;
		ti->dim = TD_GAMES;
		ti->len.games = MC_GAMES;
	}
	struct { 
		float value; 
		int   rank; 
	} top_ratio_send_buffer, top_ratio_receive_buffer;
	int my_mpi_rank;
	MPI_Comm_rank(MPI_COMM_WORLD,&my_mpi_rank);
	struct time_stop stop;
	time_stop_conditions(ti, b, 20, 40, 3.0, &stop);

	/* resign when the hope for win vanishes */
	coord_t top_coord = resign;
	floating_t top_ratio = mc->resign_ratio;

	/* We use [0] for pass. Normally, this is an inaccessible corner
	 * of board margin. */
	struct move_stat moves[board_size2(b)];
	memset(moves, 0, sizeof(moves));

	int losses = 0;
	int i, superko = 0, good_games = 0;
	bool move_not_found = true;

		struct board * boards = malloc(sizeof(struct board) * omp_thread_count);
		for (i = 0; i < omp_thread_count; i++) {
			board_copy(&boards[i], b);
		}
		unsigned long * seeds = malloc(sizeof(unsigned long) * omp_thread_count);
		for (i = 0; i < omp_thread_count; i++) {
			seeds[i] = i*3+11*my_mpi_rank+7;
			seeds[i] = random_init_omp(&seeds[i]);
		}

#pragma omp parallel \
  private(i) \
  reduction(+:losses,superko,good_games)
{
	for (i = 0; i < stop.desired.playouts && move_not_found; i++) {
		assert(!b->superko_violation);
		int thread_num = omp_get_thread_num();
		unsigned long * seed = &seeds[thread_num];


		struct board b2 = boards[thread_num];
		board_copy_noalloc(&b2, b);

		coord_t coord;

		board_play_random_omp(&b2, color, &coord, NULL, NULL, seed);
		if (!is_pass(coord) && !group_at(&b2, coord)) {
			/* Multi-stone suicide. We play chinese rules,
			 * so we can't consider this. (Note that we
			 * unfortunately still consider this in playouts.) */
			if (DEBUGL(4)) {
				fprintf(stderr, "SUICIDE DETECTED at %d,%d:\n", coord_x(coord, b), coord_y(coord, b));
				board_print(b, stderr);
			}
			continue;
		}

		if (DEBUGL(3))
			fprintf(stderr, "[%d,%d color %d] playing random game\n", coord_x(coord, b), coord_y(coord, b), color);

		struct playout_setup ps = { .gamelen = mc->gamelen };
		int result = play_random_game(&ps, &b2, color, NULL, NULL, mc->playout);


		if (result == 0) {
			/* Superko. We just ignore this playout.
			 * And play again. */
			if (unlikely(superko > 2 * stop.desired.playouts)) {
				/* Uhh. Triple ko, or something? */
				if (MCDEBUGL(0))
					fprintf(stderr, "SUPERKO LOOP. I will pass. Did we hit triple ko?\n");
				//goto pass_wins;
				#pragma omp critical(update_move_not_found_flag)
				{
					move_not_found = false;
				}
			}
			/* This playout didn't count; we should not
			 * disadvantage moves that lead to a superko.
			 * And it is supposed to be rare. */
			i--, superko++;
			continue;
		}

		if (MCDEBUGL(3))
			fprintf(stderr, "\tresult for other player: %d\n", result);

		int pos = is_pass(coord) ? 0 : coord;

		good_games++;
		moves[pos].games++;

		losses += result > 0;
		moves[pos].wins += 1 - (result > 0);

		if (unlikely(!losses && i == mc->loss_threshold)) {
			/* We played out many games and didn't lose once yet.
			 * This game is over. */
			break;
		}
	}
} //end omp
for (i = 0; i < omp_thread_count; i++) {
			board_done_noalloc(&boards[i]);
		}
		free(boards);
		free(seeds);
if(!move_not_found){
	goto pass_wins;
}

	if (!good_games) {
		/* No moves to try??? */
		if (MCDEBUGL(0)) {
			fprintf(stderr, "OUT OF MOVES! I will pass. But how did this happen?\n");
			board_print(b, stderr);
		}
pass_wins:
		top_coord = pass; top_ratio = 0.5;
		goto move_found;
	}

	foreach_point(b) {
		if (b->moves < 3) {
			/* Simple heuristic: avoid opening too low. Do not
			 * play on second or first line as first white or
			 * first two black moves.*/
			if (coord_x(c, b) < 3 || coord_x(c, b) > board_size(b) - 4
			    || coord_y(c, b) < 3 || coord_y(c, b) > board_size(b) - 4)
				continue;
		}

		floating_t ratio = (floating_t) moves[c].wins / moves[c].games;
		/* Since pass is [0,0], we will pass only when we have nothing
		 * better to do. */
		if (ratio >= top_ratio) {
			top_ratio = ratio;
			top_coord = c == 0 ? pass : c;
		}
	} foreach_point_end;

	if (MCDEBUGL(2)) {
		board_stats_print_mpi(b, moves, stderr);
	}

move_found:
	if (MCDEBUGL(1))
		fprintf(stderr, "*** WINNER is %d,%d with score %1.4f (%d games, %d superko)\n", coord_x(top_coord, b), coord_y(top_coord, b), top_ratio, i, superko);

	printf("Rank %d reporting top_ratio of %f\n",my_mpi_rank,top_ratio);
	top_ratio_send_buffer.value = top_ratio;
	top_ratio_send_buffer.rank = my_mpi_rank;
	MPI_Allreduce(&top_ratio_send_buffer, &top_ratio_receive_buffer, 1, MPI_FLOAT_INT, MPI_MAXLOC, MPI_COMM_WORLD);
	MPI_Bcast(&top_coord, 2, MPI_INT, top_ratio_receive_buffer.rank, MPI_COMM_WORLD);
	return coord_copy(top_coord);
}


struct montecarlo_mpi *
montecarlo_mpi_state_init(char *arg, struct board *b)
{
	struct montecarlo_mpi *mc = calloc2(1, sizeof(struct montecarlo_mpi));

	mc->debug_level = 1;
	mc->gamelen = MC_GAMELEN;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ",");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "debug")) {
				if (optval)
					mc->debug_level = atoi(optval);
				else
					mc->debug_level++;
			} else if (!strcasecmp(optname, "gamelen") && optval) {
				mc->gamelen = atoi(optval);
			} else if (!strcasecmp(optname, "playout") && optval) {
				char *playoutarg = strchr(optval, ':');
				if (playoutarg)
					*playoutarg++ = 0;
				if (!strcasecmp(optval, "moggy")) {
					mc->playout = playout_moggy_init(playoutarg, b, joseki_load(b->size));
				} else if (!strcasecmp(optval, "light")) {
					mc->playout = playout_light_init(playoutarg, b);
				} else {
					fprintf(stderr, "montecarlo_mpi: Invalid playout policy %s\n", optval);
				}
			} else {
				fprintf(stderr, "montecarlo_mpi: Invalid engine argument %s or missing value\n", optname);
			}
		}
	}

	if (!mc->playout)
		mc->playout = playout_light_init(NULL, b);
	mc->playout->debug_level = mc->debug_level;

	mc->resign_ratio = 0.1; /* Resign when most games are lost. */
	mc->loss_threshold = 5000; /* Stop reading if no loss encountered in first 5000 games. */

	return mc;
}


struct engine *
engine_montecarlo_mpi_init(char *arg, struct board *b)
{
	struct montecarlo_mpi *mc = montecarlo_mpi_state_init(arg, b);
	struct engine *e = calloc2(1, sizeof(struct engine));
	e->name = "montecarlo_mpi";
	e->comment = "I'm playing in Monte Carlo. When we both pass, I will consider all the stones on the board alive. If you are reading this, write 'yes'. Please bear with me at the game end, I need to fill the whole board; if you help me, we will both be happier. Filling the board will not lose points (NZ rules).";
	e->genmove = montecarlo_mpi_genmove;
	e->data = mc;

	return e;
}