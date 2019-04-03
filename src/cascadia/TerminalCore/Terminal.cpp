/********************************************************
 *                                                       *
 *   Copyright (C) Microsoft. All rights reserved.       *
 *                                                       *
 ********************************************************/

#include "pch.h"
#include "Terminal.hpp"
#include "../../terminal/parser/OutputStateMachineEngine.hpp"
#include "TerminalDispatch.hpp"
#include <unicode.hpp>
#include <argb.h>
#include "../../types/inc/utils.hpp"

#include "winrt\Microsoft.Terminal.Settings.h"

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;
using namespace Microsoft::Console::Render;

std::wstring _KeyEventsToText(std::deque<std::unique_ptr<IInputEvent>>& inEventsToWrite)
{
    std::wstring wstr = L"";
    for(auto& ev : inEventsToWrite)
    {
        if (ev->EventType() == InputEventType::KeyEvent)
        {
            auto& k = static_cast<KeyEvent&>(*ev);
            auto wch = k.GetCharData();
            wstr += wch;
        }
    }
    return wstr;
}

Terminal::Terminal() :
    _mutableViewport{Viewport::Empty()},
    _title{ L"" },
    _colorTable{},
    _defaultFg{ RGB(255, 255, 255) },
    _defaultBg{ ARGB(0, 0, 0, 0) },
    _pfnWriteInput{ nullptr },
    _scrollOffset{ 0 },
    _snapOnInput{ true },
    _boxSelection{ false },
    _renderSelection{ false }
{
    _stateMachine = std::make_unique<StateMachine>(new OutputStateMachineEngine(new TerminalDispatch(*this)));

    auto passAlongInput = [&](std::deque<std::unique_ptr<IInputEvent>>& inEventsToWrite)
    {
        if(!_pfnWriteInput) return;
        std::wstring wstr = _KeyEventsToText(inEventsToWrite);
        _pfnWriteInput(wstr);
    };

    _terminalInput = std::make_unique<TerminalInput>(passAlongInput);

    _InitializeColorTable();
}

void Terminal::Create(COORD viewportSize, SHORT scrollbackLines, IRenderTarget& renderTarget)
{
    _mutableViewport = Viewport::FromDimensions({ 0,0 }, viewportSize);
    _scrollbackLines = scrollbackLines;
    COORD bufferSize { viewportSize.X, viewportSize.Y + scrollbackLines };
    TextAttribute attr{};
    UINT cursorSize = 12;
    _buffer = std::make_unique<TextBuffer>(bufferSize, attr, cursorSize, renderTarget);
}

// Method Description:
// - Initializes the Temrinal from the given set of settings.
// Arguments:
// - settings: the set of CoreSettings we need to use to initialize the terminal
// - renderTarget: A render target the terminal can use for paint invalidation.
// Return Value:
// - <none>
void Terminal::CreateFromSettings(winrt::Microsoft::Terminal::Settings::ICoreSettings settings,
            Microsoft::Console::Render::IRenderTarget& renderTarget)
{
    _defaultFg = settings.DefaultForeground();
    _defaultBg = settings.DefaultBackground();

    for (int i = 0; i < 16; i++)
    {
        _colorTable[i] = settings.GetColorTableEntry(i);
    }

    _snapOnInput = settings.SnapOnInput();
    COORD viewportSize{ (short)settings.InitialCols(), (short)settings.InitialRows() };
    // TODO:MSFT:20642297 - Support infinite scrollback here, if HistorySize is -1
    Create(viewportSize, (short)settings.HistorySize(), renderTarget);
}

// Method Description:
// - Resize the terminal as the result of some user interaction.
// Arguments:
// - viewportSize: the new size of the viewport, in chars
// Return Value:
// - S_OK if we successfully resized the terminal, S_FALSE if there was
//      nothing to do (the viewportSize is the same as our current size), or an
//      appropriate HRESULT for failing to resize.
[[nodiscard]]
HRESULT Terminal::UserResize(const COORD viewportSize) noexcept
{
    const auto oldDimensions = _mutableViewport.Dimensions();
    if (viewportSize == oldDimensions)
    {
        return S_FALSE;
    }
    _mutableViewport = Viewport::FromDimensions({ 0, 0 }, viewportSize);
    COORD bufferSize{ viewportSize.X, viewportSize.Y + _scrollbackLines };
    RETURN_IF_FAILED(_buffer->ResizeTraditional(bufferSize));

    _NotifyScrollEvent();

    return S_OK;
}

void Terminal::Write(std::wstring_view stringView)
{
    auto lock = LockForWriting();

    _stateMachine->ProcessString(stringView.data(), stringView.size());
}

bool Terminal::SendKeyEvent(const WORD vkey,
                            const bool ctrlPressed,
                            const bool altPressed,
                            const bool shiftPressed)
{
    if (_snapOnInput && _scrollOffset != 0)
    {
        auto lock = LockForWriting();
        _scrollOffset = 0;
        _NotifyScrollEvent();
    }

    DWORD modifiers = 0
                      | (ctrlPressed? LEFT_CTRL_PRESSED : 0)
                      | (altPressed? LEFT_ALT_PRESSED : 0)
                      | (shiftPressed? SHIFT_PRESSED : 0)
                      ;
    KeyEvent keyEv{ true, 0, vkey, 0, L'\0', modifiers};
    return _terminalInput->HandleKey(&keyEv);
}

// Method Description:
// - Aquire a read lock on the terminal.
// Arguments:
// - <none>
// Return Value:
// - a shared_lock which can be used to unlock the terminal. The shared_lock
//      will release this lock when it's destructed.
[[nodiscard]]
std::shared_lock<std::shared_mutex> Terminal::LockForReading()
{
    return std::shared_lock<std::shared_mutex>(_readWriteLock);
}

// Method Description:
// - Aquire a write lock on the terminal.
// Arguments:
// - <none>
// Return Value:
// - a unique_lock which can be used to unlock the terminal. The unique_lock
//      will release this lock when it's destructed.
[[nodiscard]]
std::unique_lock<std::shared_mutex> Terminal::LockForWriting()
{
    return std::unique_lock<std::shared_mutex>(_readWriteLock);
}


Viewport Terminal::_GetMutableViewport() const noexcept
{
    return _mutableViewport;
}

short Terminal::GetBufferHeight() const noexcept
{
    return _mutableViewport.BottomExclusive();
}

// _ViewStartIndex is also the length of the scrollback
int Terminal::_ViewStartIndex() const noexcept
{
    return _mutableViewport.Top();
}

// _VisibleStartIndex is the first visible line of the buffer
int Terminal::_VisibleStartIndex() const noexcept
{
    return std::max(0, _ViewStartIndex() - _scrollOffset);
}

Viewport Terminal::_GetVisibleViewport() const noexcept
{
    const COORD origin{ 0, gsl::narrow<short>(_VisibleStartIndex()) };
    return Viewport::FromDimensions(origin,
                                    _mutableViewport.Dimensions());
}

// Writes a string of text to the buffer, then moves the cursor (and viewport)
//      in accordance with the written text.
// This method is our proverbial `WriteCharsLegacy`, and great care should be made to
//      keep it minimal and orderly, lest it become WriteCharsLegacy2ElectricBoogaloo
// TODO: MSFT 21006766
//       This needs to become stream logic on the buffer itself sooner rather than later
//       because it's otherwise impossible to avoid the Electric Boogaloo-ness here.
//       I had to make a bunch of hacks to get Japanese and emoji to work-ish.
void Terminal::_WriteBuffer(const std::wstring_view& stringView)
{
    auto& cursor = _buffer->GetCursor();
    static bool skipNewline = false;
    const Viewport bufferSize = _buffer->GetSize();

    for (size_t i = 0; i < stringView.size(); i++)
    {
        wchar_t wch = stringView[i];
        const COORD cursorPosBefore = cursor.GetPosition();
        COORD proposedCursorPosition = cursorPosBefore;
        bool notifyScroll = false;

        if (wch == UNICODE_LINEFEED)
        {
            if (skipNewline)
            {
                skipNewline = false;
                continue;
            }
            else
            {
                proposedCursorPosition.Y++;
            }
        }
        else if (wch == UNICODE_CARRIAGERETURN)
        {
            proposedCursorPosition.X = 0;
        }
        else if (wch == UNICODE_BACKSPACE)
        {
            if (cursorPosBefore.X == 0)
            {
                proposedCursorPosition.X = bufferSize.Width() - 1;
                proposedCursorPosition.Y--;
            }
            else
            {
                proposedCursorPosition.X--;
            }
        }
        else
        {
            // TODO: MSFT 21006766
            // This is not great but I need it demoable. Fix by making a buffer stream writer.
            if (wch >= 0xD800 && wch <= 0xDFFF)
            {
                OutputCellIterator it{ stringView.substr(i, 2) , _buffer->GetCurrentAttributes() };
                const auto end = _buffer->Write(it);
                const auto cellDistance = end.GetCellDistance(it);
                i += cellDistance - 1;
                proposedCursorPosition.X += gsl::narrow<SHORT>(cellDistance);
            }
            else
            {
                OutputCellIterator it{ stringView.substr(i, 1) , _buffer->GetCurrentAttributes() };
                const auto end = _buffer->Write(it);
                const auto cellDistance = end.GetCellDistance(it);
                proposedCursorPosition.X += gsl::narrow<SHORT>(cellDistance);
            }
        }

        // If we're about to scroll past the bottom of the buffer, instead cycle the buffer.
        const auto newRows = proposedCursorPosition.Y - bufferSize.Height() + 1;
        if (newRows > 0)
        {
            for(auto dy = 0; dy < newRows; dy++)
            {
                _buffer->IncrementCircularBuffer();
                proposedCursorPosition.Y--;
            }
            notifyScroll = true;
        }

        // This section is essentially equivalent to `AdjustCursorPosition`
        // Update Cursor Position
        cursor.SetPosition(proposedCursorPosition);

        const COORD cursorPosAfter = cursor.GetPosition();
        skipNewline = cursorPosAfter.Y == cursorPosBefore.Y+1;

        // Move the viewport down if the cursor moved below the viewport.
        if (cursorPosAfter.Y > _mutableViewport.BottomInclusive())
        {
            const auto newViewTop = std::max(0, cursorPosAfter.Y - (_mutableViewport.Height() - 1));
            if (newViewTop != _mutableViewport.Top())
            {
                _mutableViewport = Viewport::FromDimensions({0, gsl::narrow<short>(newViewTop)}, _mutableViewport.Dimensions());
                notifyScroll = true;
            }
        }

        if (notifyScroll)
        {
            _buffer->GetRenderTarget().TriggerRedrawAll();
            _NotifyScrollEvent();
        }
    }
}

void Terminal::UserScrollViewport(const int viewTop)
{
    const auto clampedNewTop = std::max(0, viewTop);
    const auto realTop = _ViewStartIndex();
    const auto newDelta = realTop - clampedNewTop;
    // if viewTop > realTop, we want the offset to be 0.

    _scrollOffset = std::max(0, newDelta);
    _buffer->GetRenderTarget().TriggerRedrawAll();
}

int Terminal::GetScrollOffset()
{
    return _VisibleStartIndex();
}

void Terminal::_NotifyScrollEvent()
{
    if (_pfnScrollPositionChanged)
    {
        const auto visible = _GetVisibleViewport();
        const auto top = visible.Top();
        const auto height = visible.Height();
        const auto bottom = this->GetBufferHeight();
        _pfnScrollPositionChanged(top, height, bottom);
    }
}

void Terminal::SetWriteInputCallback(std::function<void(std::wstring&)> pfn) noexcept
{
    _pfnWriteInput = pfn;
}

void Terminal::SetTitleChangedCallback(std::function<void(const std::wstring_view&)> pfn) noexcept
{
    _pfnTitleChanged = pfn;
}

void Terminal::SetScrollPositionChangedCallback(std::function<void(const int, const int, const int)> pfn) noexcept
{
    _pfnScrollPositionChanged = pfn;
}

// Method Description:
// - Record the position of the beginning of a selection
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
// Return Value:
// - N/A
void Terminal::SetSelectionAnchor(const COORD position)
{
    _selectionAnchor = position;

    // include _scrollOffset here to ensure this maps to the right spot of the original viewport
    THROW_IF_FAILED(ShortSub(_selectionAnchor.Y, gsl::narrow<SHORT>(_scrollOffset), &_selectionAnchor.Y));
    _renderSelection = true;

    SetEndSelectionPosition(position);
}

// Method Description:
// - Record the position of the end of a selection
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
// Return Value:
// - N/A
void Terminal::SetEndSelectionPosition(const COORD position)
{
    _endSelectionPosition = position;

    // include _scrollOffset here to ensure this maps to the right spot of the original viewport
    THROW_IF_FAILED(ShortSub(_endSelectionPosition.Y, gsl::narrow<SHORT>(_scrollOffset), &_endSelectionPosition.Y));
}

void Terminal::_InitializeColorTable()
{
    gsl::span<COLORREF> tableView = { &_colorTable[0], gsl::narrow<ptrdiff_t>(_colorTable.size()) };
    // First set up the basic 256 colors
    ::Microsoft::Console::Utils::Initialize256ColorTable(tableView);
    // Then use fill the first 16 values with the Campbell scheme
    ::Microsoft::Console::Utils::InitializeCampbellColorTable(tableView);
    // Then make sure all the values have an alpha of 255
    ::Microsoft::Console::Utils::SetColorTableAlpha(tableView, 0xff);
}

// Method Description:
// - Helper to determine the selected region of the buffer. Used for rendering.
// Arguments:
// - N/A
// Return Value:
// - A vector of rectangles representing the regions to select, line by line.
std::vector<SMALL_RECT> Terminal::_GetSelectionRects() const
{
    std::vector<SMALL_RECT> selectionArea;

    if (!_renderSelection) 
    {
        return selectionArea;
    }

    // NOTE: (0,0) is top-left so vertical comparison is inverted
    COORD higherCoord = (_selectionAnchor.Y <= _endSelectionPosition.Y) ? _selectionAnchor : _endSelectionPosition;
    COORD lowerCoord = (_selectionAnchor.Y > _endSelectionPosition.Y) ? _selectionAnchor : _endSelectionPosition;

    selectionArea.reserve(lowerCoord.Y - higherCoord.Y + 1);
    for (auto row = higherCoord.Y; row <= lowerCoord.Y; row++)
    {
        SMALL_RECT selectionRow;

        // Add ViewStartIndex to support scrolling
        THROW_IF_FAILED(ShortAdd(row, gsl::narrow<SHORT>(_ViewStartIndex()), &selectionRow.Top));
        THROW_IF_FAILED(ShortAdd(row, gsl::narrow<SHORT>(_ViewStartIndex()), &selectionRow.Bottom));

        if (_boxSelection)
        {
            selectionRow.Left = std::min(higherCoord.X, lowerCoord.X);
            selectionRow.Right = std::max(higherCoord.X, lowerCoord.X);
        }
        else
        {
            selectionRow.Left = (row == higherCoord.Y) ? higherCoord.X : 0;
            selectionRow.Right = (row == lowerCoord.Y) ? lowerCoord.X : _buffer->GetSize().RightInclusive();
        }

        selectionArea.emplace_back(selectionRow);
    }
    return selectionArea;
}

// Method Description:
// - enable/disable box selection (ALT + selection)
// Arguments:
// - N/A
// Return Value:
// - N/A
void Terminal::SetBoxSelection(const bool isEnabled) noexcept
{
    _boxSelection = isEnabled;
}

// Method Description:
// - clear selection data and disable rendering it
// Arguments:
// - N/A
// Return Value:
// - N/A
void Terminal::ClearSelection() noexcept
{
    _selectionAnchor = {};
    _endSelectionPosition = {};
    _renderSelection = false;
}