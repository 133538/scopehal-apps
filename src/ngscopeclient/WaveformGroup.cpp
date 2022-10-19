/***********************************************************************************************************************
*                                                                                                                      *
* glscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of WaveformGroup
 */
#include "ngscopeclient.h"
#include "WaveformGroup.h"
#include "MainWindow.h"
#include "imgui_internal.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

WaveformGroup::WaveformGroup(MainWindow* parent, const string& title)
	: m_parent(parent)
	, m_xpos(0)
	, m_width(0)
	, m_pixelsPerXUnit(0.00005)
	, m_xAxisOffset(0)
	, m_title(title)
	, m_xAxisUnit(Unit::UNIT_FS)
	, m_dragState(DRAG_STATE_NONE)
	, m_dragMarker(nullptr)
	, m_tLastMouseMove(GetTime())
	, m_timelineHeight(0)
	, m_xAxisCursorMode(X_CURSOR_NONE)
{
	m_xAxisCursorPositions[0] = 0;
	m_xAxisCursorPositions[1] = 0;
}

WaveformGroup::~WaveformGroup()
{
	Clear();
}

void WaveformGroup::Clear()
{
	LogTrace("Destroying areas\n");
	LogIndenter li;

	m_areas.clear();

	LogTrace("All areas removed\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Area management

void WaveformGroup::AddArea(shared_ptr<WaveformArea>& area)
{
	m_areas.push_back(area);

	m_parent->RefreshTimebasePropertiesDialog();
}

/**
	@brief Returns true if a channel is being dragged from any WaveformArea within the group
 */
bool WaveformGroup::IsChannelBeingDragged()
{
	for(auto a : m_areas)
	{
		if(a->IsChannelBeingDragged())
			return true;
	}
	return false;
}

/**
	@brief Returns the channel being dragged, if one exists
 */
StreamDescriptor WaveformGroup::GetChannelBeingDragged()
{
	for(auto a : m_areas)
	{
		auto stream = a->GetChannelBeingDragged();
		if(stream)
			return stream;
	}
	return StreamDescriptor(nullptr, 0);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

/**
	@brief Run the tone-mapping shader on all of our waveforms

	Called by MainWindow::ToneMapAllWaveforms() at the start of each frame if new data is ready to render
 */
void WaveformGroup::ToneMapAllWaveforms(vk::raii::CommandBuffer& cmdbuf)
{
	for(auto a : m_areas)
		a->ToneMapAllWaveforms(cmdbuf);
}

void WaveformGroup::ReferenceWaveformTextures()
{
	for(auto a : m_areas)
		a->ReferenceWaveformTextures();
}

void WaveformGroup::RenderWaveformTextures(
	vk::raii::CommandBuffer& cmdbuf,
	vector<shared_ptr<DisplayedChannel> >& channels,
	bool clearPersistence)
{
	bool clearThisGroupOnly = m_clearPersistence.exchange(false);

	for(auto a : m_areas)
		a->RenderWaveformTextures(cmdbuf, channels, clearThisGroupOnly || clearPersistence);
}

bool WaveformGroup::Render()
{
	bool open = true;
	ImGui::SetNextWindowSize(ImVec2(320, 240), ImGuiCond_Appearing);
	if(!ImGui::Begin(m_title.c_str(), &open))
	{
		//tabbed out, don't draw anything until we're back in the foreground
		ImGui::End();
		return true;
	}

	auto pos = ImGui::GetCursorScreenPos();
	ImVec2 clientArea = ImGui::GetContentRegionMax();
	m_width = clientArea.x;

	//Render the timeline
	m_timelineHeight = 2.5 * ImGui::GetFontSize();
	clientArea.y -= m_timelineHeight;
	float yAxisWidthSpaced = GetYAxisWidth() + GetSpacing();
	float plotWidth = clientArea.x - yAxisWidthSpaced;
	RenderTimeline(plotWidth, m_timelineHeight);

	//Close any areas that we destroyed last frame
	//Block until all background processing completes to ensure no command buffers are still pending
	if(!m_areasToClose.empty())
	{
		g_vkComputeDevice->waitIdle();
		m_areasToClose.clear();
	}

	//Render our waveform areas
	//TODO: waveform areas full of protocol or digital decodes should be fixed size while analog will fill the gap?
	//Anything we closed is removed from the list THIS frame, so we stop rendering to them etc
	//but not actually destroyed until next frame
	for(size_t i=0; i<m_areas.size(); i++)
	{
		if(!m_areas[i]->Render(i, m_areas.size(), clientArea))
			m_areasToClose.push_back(i);
	}
	for(ssize_t i=static_cast<ssize_t>(m_areasToClose.size()) - 1; i >= 0; i--)
		m_areas.erase(m_areas.begin() + m_areasToClose[i]);
	if(!m_areasToClose.empty())
		m_parent->RefreshTimebasePropertiesDialog();

	//If we no longer have any areas in the group, close the group
	if(m_areas.empty())
		open = false;

	//Render cursors over everything else
	ImVec2 plotSize(plotWidth, clientArea.y);
	RenderXAxisCursors(pos, plotSize);
	if(m_xAxisCursorMode != X_CURSOR_NONE)
		DoCursorReadouts();
	RenderMarkers(pos, plotSize);

	ImGui::End();
	return open;
}

/**
	@brief Run the popup window with cursor values
 */
void WaveformGroup::DoCursorReadouts()
{
	bool hasSecondCursor = (m_xAxisCursorMode == X_CURSOR_DUAL);

	string name = string("Cursors (") + m_title + ")";
	float width = ImGui::GetFontSize();
	ImGui::SetNextWindowSize(ImVec2(38*width, 15*width), ImGuiCond_Appearing);
	if(ImGui::Begin(name.c_str(), nullptr, ImGuiWindowFlags_NoCollapse))
	{
		static ImGuiTableFlags flags =
			ImGuiTableFlags_Resizable |
			ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_BordersV |
			ImGuiTableFlags_ScrollY;

		//Add columns for second cursor if enabled
		int ncols = 2;
		if(hasSecondCursor)
			ncols += 2;

		if(ImGui::BeginTable("cursors", ncols, flags))
		{
			//Header row
			ImGui::TableSetupScrollFreeze(0, 1); 	//Header row does not scroll
			ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 10*width);
			ImGui::TableSetupColumn("Value 1", ImGuiTableColumnFlags_WidthFixed, 8*width);
			if(hasSecondCursor)
			{
				ImGui::TableSetupColumn("Value 2", ImGuiTableColumnFlags_WidthFixed, 8*width);
				ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthFixed, 8*width);
			}
			ImGui::TableHeadersRow();

			//Readout for each channel in all of our waveform areas
			for(auto a : m_areas)
			{
				for(size_t i=0; i<a->GetStreamCount(); i++)
				{
					auto stream = a->GetStream(i);
					auto sname = stream.GetName();

					//Fetch the values for each cursor
					auto data = stream.GetData();
					bool zhold = (stream.GetFlags() & Stream::STREAM_DO_NOT_INTERPOLATE) ? true : false;
					auto v1 = GetValueAtTime(data, m_xAxisCursorPositions[0], zhold);
					auto v2 = GetValueAtTime(data, m_xAxisCursorPositions[1], zhold);

					ImGui::PushID(sname.c_str());
					ImGui::TableNextRow(ImGuiTableRowFlags_None, 0);

					//Channel name
					ImGui::TableSetColumnIndex(0);
					auto color = ColorFromString(stream.m_channel->m_displaycolor);
					ImGui::PushStyleColor(ImGuiCol_Text, color);
					ImGui::TextUnformatted(sname.c_str());
					ImGui::PopStyleColor();

					//Cursor 0 value
					ImGui::TableSetColumnIndex(1);
					if(!v1)
						RightJustifiedText("(no data)");
					else
						RightJustifiedText(stream.GetYAxisUnits().PrettyPrint(v1.value()));

					if(hasSecondCursor)
					{
						//Cursor 1 value
						ImGui::TableSetColumnIndex(2);
						if(!v2)
							RightJustifiedText("(no data)");
						else
							RightJustifiedText(stream.GetYAxisUnits().PrettyPrint(v2.value()));

						//Delta
						ImGui::TableSetColumnIndex(3);
						if(!v1 || !v2)
							RightJustifiedText("(no data)");
						else
							RightJustifiedText(stream.GetYAxisUnits().PrettyPrint(v2.value() - v1.value()));
					}

					ImGui::PopID();
				}
			}

			ImGui::EndTable();
		}
	}
	ImGui::End();
}

/**
	@brief Render our markers
 */
void WaveformGroup::RenderMarkers(ImVec2 pos, ImVec2 size)
{
	//Don't draw anything if our unit isn't fs
	//TODO: support units for frequency domain channels etc?
	//TODO: early out if eye pattern
	if(m_xAxisUnit != Unit(Unit::UNIT_FS))
		return;

	auto& markers = m_parent->GetSession().GetMarkers(m_areas[0]->GetWaveformTimestamp());

	//Create a child window for all of our drawing
	//(this is needed so we're above the WaveformArea's in z order, but behind popup windows)
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	if(ImGui::BeginChild("markers", size, false, ImGuiWindowFlags_NoInputs))
	{
		auto list = ImGui::GetWindowDrawList();

		auto& prefs = m_parent->GetSession().GetPreferences();
		auto color = prefs.GetColor("Appearance.Cursors.marker_color");
		auto font = m_parent->GetFontPref("Appearance.Cursors.label_font");

		//Draw the markers
		for(auto& m : markers)
		{
			//Lines
			float xpos = round(XAxisUnitsToXPosition(m.m_offset));
			list->AddLine(ImVec2(xpos, pos.y), ImVec2(xpos, pos.y + size.y), color);

			//Text
			//Anchor bottom right at the cursor
			auto str = m.m_name + ": " + m_xAxisUnit.PrettyPrint(m.m_offset);
			auto tsize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0, str.c_str());
			float padding = 2;
			float wrounding = 2;
			float textTop = pos.y + m_timelineHeight - (padding + tsize.y);
			list->AddRectFilled(
				ImVec2(xpos - (2*padding + tsize.x), textTop - padding ),
				ImVec2(xpos - 1, pos.y + m_timelineHeight),
				ImGui::GetColorU32(ImGuiCol_PopupBg),
				wrounding);
			list->AddText(
				font,
				font->FontSize,
				ImVec2(xpos - (padding + tsize.x), textTop),
				color,
				str.c_str());
		}
	}
	ImGui::EndChild();

	auto mouse = ImGui::GetMousePos();
	for(auto& m : markers)
	{
		//Child window doesn't get mouse events (this flag is needed so we can pass mouse events to the WaveformArea's)
		//So we have to do all of our interaction processing inside the top level window
		//TODO: this is basically DoCursor(), can we de-duplicate this code?
		float xpos = round(XAxisUnitsToXPosition(m.m_offset));
		float searchRadius = 0.25 * ImGui::GetFontSize();

		//Check if the mouse hit us
		if(ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
		{
			if( fabs(mouse.x - xpos) < searchRadius)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

				//Start dragging if clicked
				if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					m_dragState = DRAG_STATE_MARKER;
					m_dragMarker = &m;
				}
			}
		}
	}

	//If dragging, move the cursor to track
	if(m_dragState == DRAG_STATE_MARKER)
	{
		if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			m_dragState = DRAG_STATE_NONE;
		m_dragMarker->m_offset = XPositionToXAxisUnits(mouse.x);
	}
}

/**
	@brief Render our cursors
 */
void WaveformGroup::RenderXAxisCursors(ImVec2 pos, ImVec2 size)
{
	//No cursors? Nothing to do
	if(m_xAxisCursorMode == X_CURSOR_NONE)
		return;

	//Create a child window for all of our drawing
	//(this is needed so we're above the WaveformArea's in z order, but behind popup windows)
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	if(ImGui::BeginChild("cursors", size, false, ImGuiWindowFlags_NoInputs))
	{
		auto list = ImGui::GetWindowDrawList();

		auto& prefs = m_parent->GetSession().GetPreferences();
		auto cursor0_color = prefs.GetColor("Appearance.Cursors.cursor_1_color");
		auto cursor1_color = prefs.GetColor("Appearance.Cursors.cursor_2_color");
		auto fill_color = prefs.GetColor("Appearance.Cursors.cursor_fill_color");
		auto font = m_parent->GetFontPref("Appearance.Cursors.label_font");

		float xpos0 = round(XAxisUnitsToXPosition(m_xAxisCursorPositions[0]));
		float xpos1 = round(XAxisUnitsToXPosition(m_xAxisCursorPositions[1]));

		//Fill between if dual cursor
		if(m_xAxisCursorMode == X_CURSOR_DUAL)
			list->AddRectFilled(ImVec2(xpos0, pos.y), ImVec2(xpos1, pos.y + size.y), fill_color);

		//First cursor
		list->AddLine(ImVec2(xpos0, pos.y), ImVec2(xpos0, pos.y + size.y), cursor0_color, 1);

		//Text
		//Anchor bottom right at the cursor
		auto str = string("X1: ") + m_xAxisUnit.PrettyPrint(m_xAxisCursorPositions[0]);
		auto tsize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0, str.c_str());
		float padding = 2;
		float wrounding = 2;
		float textTop = pos.y + m_timelineHeight - (padding + tsize.y);
		list->AddRectFilled(
			ImVec2(xpos0 - (2*padding + tsize.x), textTop - padding ),
			ImVec2(xpos0 - 1, pos.y + m_timelineHeight),
			ImGui::GetColorU32(ImGuiCol_PopupBg),
			wrounding);
		list->AddText(
			font,
			font->FontSize,
			ImVec2(xpos0 - (padding + tsize.x), textTop),
			cursor0_color,
			str.c_str());

		//Second cursor
		if(m_xAxisCursorMode == X_CURSOR_DUAL)
		{
			list->AddLine(ImVec2(xpos1, pos.y), ImVec2(xpos1, pos.y + size.y), cursor1_color, 1);

			int64_t delta = m_xAxisCursorPositions[1] - m_xAxisCursorPositions[0];
			str = string("X2: ") + m_xAxisUnit.PrettyPrint(m_xAxisCursorPositions[1]) + "\n" +
				"ΔX = " + m_xAxisUnit.PrettyPrint(delta);

			//If X axis is time domain, show frequency dual
			Unit hz(Unit::UNIT_HZ);
			if(m_xAxisUnit.GetType() == Unit::UNIT_FS)
				str += string(" (") + hz.PrettyPrint(FS_PER_SECOND / delta) + ")";

			//Text
			tsize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0, str.c_str());
			textTop = pos.y + m_timelineHeight - (padding + tsize.y);
			list->AddRectFilled(
				ImVec2(xpos1 + 1, textTop - padding ),
				ImVec2(xpos1 + (2*padding + tsize.x), pos.y + m_timelineHeight),
				ImGui::GetColorU32(ImGuiCol_PopupBg),
				wrounding);
			list->AddText(
				font,
				font->FontSize,
				ImVec2(xpos1 + padding, textTop),
				cursor1_color,
				str.c_str());
		}

		//TODO: text for value readouts, in-band power, etc
	}
	ImGui::EndChild();

	//Child window doesn't get mouse events (this flag is needed so we can pass mouse events to the WaveformArea's)
	//So we have to do all of our interaction processing inside the top level window
	DoCursor(0, DRAG_STATE_X_CURSOR0);
	if(m_xAxisCursorMode == X_CURSOR_DUAL)
		DoCursor(1, DRAG_STATE_X_CURSOR1);

	//If not currently dragging, a click places cursor 0 and starts dragging cursor 1 (if enabled)
	if( ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		(m_dragState == DRAG_STATE_NONE) &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_xAxisCursorPositions[0] = XPositionToXAxisUnits(ImGui::GetMousePos().x);
		if(m_xAxisCursorMode == X_CURSOR_DUAL)
		{
			m_dragState = DRAG_STATE_X_CURSOR1;
			m_xAxisCursorPositions[1] = m_xAxisCursorPositions[0];
		}
		else
			m_dragState = DRAG_STATE_X_CURSOR0;
	}

	//Cursor 0 should always be left of cursor 1 (if both are enabled).
	//If they get swapped, exchange them.
	if( (m_xAxisCursorPositions[0] > m_xAxisCursorPositions[1]) && (m_xAxisCursorMode == X_CURSOR_DUAL) )
	{
		//Swap the cursors themselves
		int64_t tmp = m_xAxisCursorPositions[0];
		m_xAxisCursorPositions[0] = m_xAxisCursorPositions[1];
		m_xAxisCursorPositions[1] = tmp;

		//If dragging one cursor, switch to dragging the other
		if(m_dragState == DRAG_STATE_X_CURSOR0)
			m_dragState = DRAG_STATE_X_CURSOR1;
		else if(m_dragState == DRAG_STATE_X_CURSOR1)
			m_dragState = DRAG_STATE_X_CURSOR0;
	}
}

void WaveformGroup::DoCursor(int iCursor, DragState state)
{
	float xpos = round(XAxisUnitsToXPosition(m_xAxisCursorPositions[iCursor]));
	float searchRadius = 0.25 * ImGui::GetFontSize();

	//Check if the mouse hit us
	auto mouse = ImGui::GetMousePos();
	if(ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	{
		if( fabs(mouse.x - xpos) < searchRadius)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

			//Start dragging if clicked
			if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				m_dragState = state;
		}
	}

	//If dragging, move the cursor to track
	if(m_dragState == state)
	{
		if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			m_dragState = DRAG_STATE_NONE;
		m_xAxisCursorPositions[iCursor] = XPositionToXAxisUnits(mouse.x);
	}
}

void WaveformGroup::RenderTimeline(float width, float height)
{
	ImGui::BeginChild("timeline", ImVec2(width, height));

	//TODO: handle mouse wheel on the timeline

	auto list = ImGui::GetWindowDrawList();

	//Style settings
	auto& prefs = m_parent->GetSession().GetPreferences();
	auto color = prefs.GetColor("Appearance.Timeline.axis_color");
	auto textcolor = prefs.GetColor("Appearance.Timeline.text_color");
	auto font = m_parent->GetFontPref("Appearance.Timeline.x_axis_font");
	float fontSize = font->FontSize;

	//Reserve an empty area for the timeline
	auto pos = ImGui::GetWindowPos();
	m_xpos = pos.x;
	ImGui::Dummy(ImVec2(width, height));

	//Detect mouse movement
	double tnow = GetTime();
	auto mouseDelta = ImGui::GetIO().MouseDelta;
	if( (mouseDelta.x != 0) || (mouseDelta.y != 0) )
		m_tLastMouseMove = tnow;

	//Help tooltip
	//Only show if mouse has been still for 250ms
	if( (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) && (tnow - m_tLastMouseMove > 0.25) )
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 50);
		ImGui::TextUnformatted(
			"Click and drag to scroll the timeline.\n"
			"Use mouse wheel to zoom.\n"
			"Middle click to zoom to fit the entire waveform.\n"
			"Double-click to open timebase properties.");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	ImGui::SetItemUsingMouseWheel();
	if(ImGui::IsItemHovered())
	{
		//Catch mouse wheel events
		auto wheel = ImGui::GetIO().MouseWheel;
		if(wheel != 0)
			OnMouseWheel(wheel);

		//Double click to open the timebase properties
		if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			m_parent->ShowTimebaseProperties();

		//Start dragging
		if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			m_dragState = DRAG_STATE_TIMELINE;

		//Autoscale on middle mouse
		if(ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			LogTrace("middle mouse autoscale\n");

			//Find beginning and end of all waveforms in the group
			int64_t start = INT64_MAX;
			int64_t end = -INT64_MAX;
			for(auto a : m_areas)
			{
				for(size_t i=0; i<a->GetStreamCount(); i++)
				{
					auto stream = a->GetStream(i);
					auto data = stream.GetData();
					if(data == nullptr)
						continue;
					auto sdata = dynamic_cast<SparseWaveformBase*>(data);
					auto udata = dynamic_cast<UniformWaveformBase*>(data);

					int64_t wstart = GetOffsetScaled(sdata, udata, 0);
					int64_t wend =
						GetOffsetScaled(sdata, udata, data->size()-1) +
						GetDurationScaled(sdata, udata, data->size()-1);

					start = min(start, wstart);
					end = max(end, wend);
				}
			}
			int64_t sigwidth = end - start;

			//Don't divide by zero if no data!
			if(sigwidth > 1)
			{
				m_pixelsPerXUnit = width / sigwidth;
				m_xAxisOffset = start;
				ClearPersistence();
			}
		}
	}

	//Handle dragging
	//(Mouse is allowed to leave the window, as long as original click was within us)
	if(m_dragState == DRAG_STATE_TIMELINE)
	{
		//Use relative delta, not drag delta, since we update the offset every frame
		float dx = mouseDelta.x * ImGui::GetWindowDpiScale();
		if(dx != 0)
		{
			m_xAxisOffset -= PixelsToXAxisUnits(dx);
			ClearPersistence();
		}

		if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			m_dragState = DRAG_STATE_NONE;
	}

	//Dimensions for various things
	float dpiScale = ImGui::GetWindowDpiScale();
	float fineTickLength = 10 * dpiScale;
	float coarseTickLength = height;
	const double min_label_grad_width = 75 * dpiScale;	//Minimum distance between text labels
	float thickLineWidth = 2;
	float thinLineWidth = 1;
	float ymid = pos.y + height/2;

	//Top line
	list->PathLineTo(pos);
	list->PathLineTo(ImVec2(pos.x + width, pos.y));
	list->PathStroke(color, 0, thickLineWidth);

	//Figure out rounding granularity, based on our time scales
	float xscale = m_pixelsPerXUnit;
	int64_t width_xunits = width / xscale;
	auto round_divisor = GetRoundingDivisor(width_xunits);

	//Figure out about how much time per graduation to use
	double grad_xunits_nominal = min_label_grad_width / xscale;

	//Round so the division sizes are sane
	double units_per_grad = grad_xunits_nominal * 1.0 / round_divisor;
	double base = 5;
	double log_units = log(units_per_grad) / log(base);
	double log_units_rounded = ceil(log_units);
	double units_rounded = pow(base, log_units_rounded);
	float textMargin = 2;
	int64_t grad_xunits_rounded = round(units_rounded * round_divisor);

	//avoid divide-by-zero in weird cases with no waveform etc
	if(grad_xunits_rounded == 0)
		return;

	//Calculate number of ticks within a division
	double nsubticks = 5;
	double subtick = grad_xunits_rounded / nsubticks;

	//Find the start time (rounded as needed)
	double tstart = round(m_xAxisOffset / grad_xunits_rounded) * grad_xunits_rounded;

	//Print tick marks and labels
	for(double t = tstart; t < (tstart + width_xunits + grad_xunits_rounded); t += grad_xunits_rounded)
	{
		double x = (t - m_xAxisOffset) * xscale;

		//Draw fine ticks first (even if the labeled graduation doesn't fit)
		for(int tick=1; tick < nsubticks; tick++)
		{
			double subx = (t - m_xAxisOffset + tick*subtick) * xscale;

			if(subx < 0)
				continue;
			if(subx > width)
				break;
			subx += pos.x;

			list->PathLineTo(ImVec2(subx, pos.y));
			list->PathLineTo(ImVec2(subx, pos.y + fineTickLength));
			list->PathStroke(color, 0, thinLineWidth);
		}

		if(x < 0)
			continue;
		if(x > width)
			break;

		//Coarse ticks
		x += pos.x;
		list->PathLineTo(ImVec2(x, pos.y));
		list->PathLineTo(ImVec2(x, pos.y + coarseTickLength));
		list->PathStroke(color, 0, thickLineWidth);

		//Render label
		list->AddText(
			font,
			fontSize,
			ImVec2(x + textMargin, ymid),
			textcolor,
			m_xAxisUnit.PrettyPrint(t).c_str());
	}

	ImGui::EndChild();
}

/**
	@brief Handles a mouse wheel scroll step
 */
void WaveformGroup::OnMouseWheel(float delta)
{
	//TODO: if shift is held, scroll horizontally

	int64_t target = XPositionToXAxisUnits(ImGui::GetIO().MousePos.x);

	//Zoom in
	if(delta > 0)
		OnZoomInHorizontal(target, pow(1.5, delta));
	else
		OnZoomOutHorizontal(target, pow(1.5, -delta));
}

/**
	@brief Decide on reasonable rounding intervals for X axis scale ticks
 */
int64_t WaveformGroup::GetRoundingDivisor(int64_t width_xunits)
{
	int64_t round_divisor = 1;

	if(width_xunits < 1E7)
	{
		//fs, leave default
		if(width_xunits < 1e2)
			round_divisor = 1e1;
		else if(width_xunits < 1e5)
			round_divisor = 1e4;
		else if(width_xunits < 5e5)
			round_divisor = 5e4;
		else if(width_xunits < 1e6)
			round_divisor = 1e5;
		else if(width_xunits < 2.5e6)
			round_divisor = 2.5e5;
		else if(width_xunits < 5e6)
			round_divisor = 5e5;
		else
			round_divisor = 1e6;
	}
	else if(width_xunits < 1e9)
		round_divisor = 1e6;
	else if(width_xunits < 1e12)
	{
		if(width_xunits < 1e11)
			round_divisor = 1e8;
		else
			round_divisor = 1e9;
	}
	else if(width_xunits < 1E14)
		round_divisor = 1E12;
	else
		round_divisor = 1E15;

	return round_divisor;
}

/**
	@brief Clear saved persistence waveforms
 */
void WaveformGroup::ClearPersistence()
{
	m_parent->SetNeedRender();
	m_clearPersistence = true;
}

/**
	@brief Clear saved persistence waveforms of any WaveformArea's within this group containing a stream of one channel

	Typically called when a channel is reconfigured.
 */
void WaveformGroup::ClearPersistenceOfChannel(OscilloscopeChannel* chan)
{
	for(auto a : m_areas)
		a->ClearPersistenceOfChannel(chan);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Zooming

/**
	@brief Zoom in, keeping timestamp "target" at the same pixel position
 */
void WaveformGroup::OnZoomInHorizontal(int64_t target, float step)
{
	//Calculate the *current* position of the target within the window
	float delta = target - m_xAxisOffset;

	//Change the zoom
	m_pixelsPerXUnit *= step;
	m_xAxisOffset = target - (delta/step);

	ClearPersistence();
}

void WaveformGroup::OnZoomOutHorizontal(int64_t target, float step)
{
	//TODO: Clamp to bounds of all waveforms in the group
	//(not width of single widest waveform, as they may have different offsets)

	//Calculate the *current* position of the target within the window
	float delta = target - m_xAxisOffset;

	//Change the zoom
	m_pixelsPerXUnit /= step;
	m_xAxisOffset = target - (delta*step);

	ClearPersistence();
}

/**
	@brief Scrolls the group so the specified tiestamp is visible
 */
void WaveformGroup::NavigateToTimestamp(int64_t timestamp)
{
	//If X axis unit is not fs, don't scroll
	if(m_xAxisUnit != Unit(Unit::UNIT_FS))
		return;

	//TODO: support markers with other units? how to handle that?
	//TODO: early out if eye pattern

	m_xAxisOffset = timestamp - 0.5*(m_width / m_pixelsPerXUnit);
}
