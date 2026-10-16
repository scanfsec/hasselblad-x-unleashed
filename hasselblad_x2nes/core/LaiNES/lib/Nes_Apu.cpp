
// Nes_Snd_Emu 0.1.7. http://www.slack.net/~ant/libs/

#include "Nes_Apu.h"

/* Copyright (C) 2003-2005 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
more details. You should have received a copy of the GNU Lesser General
Public License along with this module; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */

#include BLARGG_SOURCE_BEGIN

Nes_Apu::Nes_Apu()
{
	dmc.apu = this;
	dmc.rom_reader = NULL;
	square1.synth = &square_synth;
	square2.synth = &square_synth;
	irq_notifier_ = NULL;
	
	oscs [0] = &square1;
	oscs [1] = &square2;
	oscs [2] = &triangle;
	oscs [3] = &noise;
	oscs [4] = &dmc;
	
	output( NULL );
	volume( 1.0 );
	reset( false );
}

Nes_Apu::~Nes_Apu()
{
}

void Nes_Apu::treble_eq( const blip_eq_t& eq )
{
	square_synth.treble_eq( eq );
	triangle.synth.treble_eq( eq );
	noise.synth.treble_eq( eq );
	dmc.synth.treble_eq( eq );
}

void Nes_Apu::buffer_cleared()
{
	square1.last_amp = 0;
	square2.last_amp = 0;
	triangle.last_amp = 0;
	noise.last_amp = 0;
	dmc.last_amp = 0;
}

void Nes_Apu::enable_nonlinear( double v )
{
	dmc.nonlinear = true;
	square_synth.volume( 1.3 * 0.25751258 / 0.742467605 * 0.25 * v );
	
	const double tnd = 0.75 / 202 * 0.48;
	triangle.synth.volume_unit( 3 * tnd );
	noise.synth.volume_unit( 2 * tnd );
	dmc.synth.volume_unit( tnd );
	
	buffer_cleared();
}

void Nes_Apu::volume( double v )
{
	dmc.nonlinear = false;
	square_synth.volume( 0.1128 * v );
	triangle.synth.volume( 0.12765 * v );
	noise.synth.volume( 0.0741 * v );
	dmc.synth.volume( 0.42545 * v );
}

void Nes_Apu::output( Blip_Buffer* buffer )
{
	for ( int i = 0; i < osc_count; i++ )
		osc_output( i, buffer );
}

void Nes_Apu::reset( bool pal_mode, int initial_dmc_dac )
{
	// to do: time pal frame periods exactly
	frame_period = pal_mode ? 8314 : 7457;  // First step timing
	dmc.pal_mode = pal_mode;
	
	square1.reset();
	square2.reset();
	triangle.reset();
	noise.reset();
	dmc.reset();
	
	last_time = 0;
	osc_enables = 0;
	irq_flag = false;
	earliest_irq_ = no_irq;
	frame_delay = 1;

	// Initialize frame counter state
	frame_counter_cycles = 0;
	frame_counter_step = 0;
	frame_counter_reset_delay = 0;
	for (int i = 0; i < 5; i++)
		frame_step_clocked[i] = false;

	write_register( 0, 0x4017, 0x00, true );
	write_register( 0, 0x4015, 0x00, true );

	for ( cpu_addr_t addr = start_addr; addr <= 0x4013; addr++ )
		write_register( 0, addr, (addr & 3) ? 0x00 : 0x10, true );
	
	dmc.dac = initial_dmc_dac;
	if ( !dmc.nonlinear )
		dmc.last_amp = initial_dmc_dac; // prevent output transition
}

void Nes_Apu::irq_changed()
{
	cpu_time_t new_irq = dmc.next_irq;
	if ( dmc.irq_flag | irq_flag ) {
		new_irq = 0;
	}
	else if ( new_irq > next_irq ) {
		new_irq = next_irq;
	}
	
	if ( new_irq != earliest_irq_ ) {
		earliest_irq_ = new_irq;
		if ( irq_notifier_ )
			irq_notifier_( irq_data );
	}
}

// frames

// Accurate frame counter timing tables (in CPU cycles from reset)
static const int frame_counter_4step_times[] = { 7457, 14913, 22371, 29829 };
static const int frame_counter_5step_times[] = { 7457, 14913, 22371, 29829, 37281 };

void Nes_Apu::run_until( cpu_time_t end_time )
{
	require( end_time >= last_time );

	if ( end_time == last_time )
		return;

	// Process time in chunks
	cpu_time_t time = last_time;
	while ( time < end_time )
	{
		cpu_time_t chunk_end = end_time;

		// Handle frame counter reset delay
		if ( frame_counter_reset_delay > 0 )
		{
			int delay_end = time + frame_counter_reset_delay;
			if ( delay_end <= end_time )
			{
				// Reset will occur during this time period
				chunk_end = delay_end;
			}
		}

		// Run oscillators for this chunk
		square1.run( time, chunk_end );
		square2.run( time, chunk_end );
		triangle.run( time, chunk_end );
		noise.run( time, chunk_end );
		dmc.run( time, chunk_end );

		// Update frame counter
		if ( frame_counter_reset_delay > 0 )
		{
			frame_counter_reset_delay -= (chunk_end - time);
			if ( frame_counter_reset_delay <= 0 )
			{
				// Frame counter reset occurs
				frame_counter_cycles = -frame_counter_reset_delay; // Account for overshoot
				frame_counter_reset_delay = 0;
				frame_counter_step = 0;
				for (int i = 0; i < 5; i++)
					frame_step_clocked[i] = false;
			}
		}
		else
		{
			// Normal frame counter operation
			int old_cycles = frame_counter_cycles;
			frame_counter_cycles += (chunk_end - time);

			bool mode5 = (frame_mode & 0x80) != 0;
			const int* step_times = mode5 ? frame_counter_5step_times : frame_counter_4step_times;
			int max_steps = mode5 ? 5 : 4;

			// Check if we've reached any step times
			for (int step = 0; step < max_steps; step++)
			{
				if (!frame_step_clocked[step] && frame_counter_cycles >= step_times[step])
				{
					frame_step_clocked[step] = true;

					// Clock envelope and linear counter every step
					triangle.clock_linear_counter();
					square1.clock_envelope();
					square2.clock_envelope();
					noise.clock_envelope();

					// Clock length and sweep on specific steps
					if ((!mode5 && (step == 1 || step == 3)) ||
					    (mode5 && (step == 0 || step == 2)))
					{
						square1.clock_length( 0x20 );
						square2.clock_length( 0x20 );
						noise.clock_length( 0x20 );
						triangle.clock_length( 0x80 );

						square1.clock_sweep( -1 );
						square2.clock_sweep( 0 );
					}
				}
			}

			// Handle IRQ in 4-step mode
			if (!mode5 && !(frame_mode & 0x40))  // 4-step mode with IRQ enabled
			{
				// IRQ occurs at cycle 29830 (one cycle after the last step)
				if (old_cycles < 29830 && frame_counter_cycles >= 29830)
				{
					irq_flag = true;
					next_irq = chunk_end;
					irq_changed();
				}
			}

			// Reset sequence after completing all steps
			if (!mode5 && frame_counter_cycles >= 29830)
			{
				// 4-step mode wraps at 29830
				frame_counter_cycles -= 29830;
				for (int i = 0; i < 5; i++)
					frame_step_clocked[i] = false;
			}
			else if (mode5 && frame_counter_cycles >= 37282)
			{
				// 5-step mode wraps at 37282
				frame_counter_cycles -= 37282;
				for (int i = 0; i < 5; i++)
					frame_step_clocked[i] = false;
			}
		}

		time = chunk_end;
	}

	last_time = end_time;
}

void Nes_Apu::end_frame( cpu_time_t end_time )
{
	if ( end_time > last_time )
		run_until( end_time );

	// make times relative to new frame
	last_time -= end_time;
	require( last_time >= 0 );

	// Don't adjust frame_counter_cycles - it's absolute from reset, not relative

	if ( next_irq != no_irq ) {
		next_irq -= end_time;
		if ( next_irq < 0 )
			next_irq = 0;
	}
	if ( dmc.next_irq != no_irq ) {
		dmc.next_irq -= end_time;
		assert( dmc.next_irq >= 0 );
	}
	if ( earliest_irq_ != no_irq ) {
		earliest_irq_ -= end_time;
		if ( earliest_irq_ < 0 )
			earliest_irq_ = 0;
	}
}

// registers

static const unsigned char length_table [0x20] = {
	0x0A, 0xFE, 0x14, 0x02, 0x28, 0x04, 0x50, 0x06,
	0xA0, 0x08, 0x3C, 0x0A, 0x0E, 0x0C, 0x1A, 0x0E, 
	0x0C, 0x10, 0x18, 0x12, 0x30, 0x14, 0x60, 0x16,
	0xC0, 0x18, 0x48, 0x1A, 0x10, 0x1C, 0x20, 0x1E
};

void Nes_Apu::write_register( cpu_time_t time, cpu_addr_t addr, int data, bool is_put_cycle )
{
	require( addr > 0x20 ); // addr must be actual address (i.e. 0x40xx)
	require( (unsigned) data <= 0xff );
	
	// Ignore addresses outside range
	if ( addr < start_addr || end_addr < addr )
		return;
	
	run_until( time );
	
	if ( addr < 0x4014 )
	{
		// Write to channel
		int osc_index = (addr - start_addr) >> 2;
		Nes_Osc* osc = oscs [osc_index];
		
		int reg = addr & 3;
		osc->regs [reg] = data;
		osc->reg_written [reg] = true;
		
		if ( osc_index == 4 )
		{
			// handle DMC specially
			dmc.write_register( reg, data );
		}
		else if ( reg == 3 )
		{
			// load length counter
			if ( (osc_enables >> osc_index) & 1 )
				osc->length_counter = length_table [(data >> 3) & 0x1f];
			
			// reset square phase
			if ( osc_index < 2 )
				((Nes_Square*) osc)->phase = Nes_Square::phase_range - 1;
		}
	}
	else if ( addr == 0x4015 )
	{
		// Channel enables
		for ( int i = osc_count; i--; )
			if ( !((data >> i) & 1) )
				oscs [i]->length_counter = 0;
		
		bool recalc_irq = dmc.irq_flag;
		dmc.irq_flag = false;
		
		int old_enables = osc_enables;
		osc_enables = data;
		if ( !(data & 0x10) ) {
			dmc.next_irq = no_irq;
			recalc_irq = true;
		}
		else if ( !(old_enables & 0x10) ) {
			dmc.start(); // dmc just enabled
		}
		
		if ( recalc_irq )
			irq_changed();
	}
	else if ( addr == 0x4017 )
	{
		// Frame mode
		frame_mode = data;

		bool irq_enabled = !(data & 0x40);
		irq_flag &= irq_enabled;
		next_irq = no_irq;

		// Reset frame counter with delay
		// The reset occurs on odd cycles only, after the first even cycle
		// Timing: 2 cycles on odd cycle write, 3 cycles on even cycle write
		frame_counter_reset_delay = (time & 1) ? 2 : 3;

		if ( data & 0x80 )
		{
			// 5-step mode - immediately clock all units
			// This happens IMMEDIATELY, not after the reset delay
			square1.clock_length( 0x20 );
			square2.clock_length( 0x20 );
			noise.clock_length( 0x20 );
			triangle.clock_length( 0x80 );

			square1.clock_sweep( -1 );
			square2.clock_sweep( 0 );

			// Also clock envelopes and linear counter
			triangle.clock_linear_counter();
			square1.clock_envelope();
			square2.clock_envelope();
			noise.clock_envelope();
		}

		irq_changed();
	}
}

int Nes_Apu::read_status( cpu_time_t time, bool is_put_cycle )
{
	run_until( time - 1 );
	
	int result = (dmc.irq_flag << 7) | (irq_flag << 6);
	
	for ( int i = 0; i < osc_count; i++ )
		if ( oscs [i]->length_counter )
			result |= 1 << i;
	
	run_until( time );

	// Reading $4015 should always clear the IRQ flag
	if ( irq_flag ) {
		irq_flag = false;
		irq_changed();
	}
	
	return result;
}

