// Copyright 2024-2026 Rive, Inc. All rights reserved.

#include "Slate/SRiveLeafWidget.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#endif
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "IRiveRendererModule.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RiveRenderer.h"
#include "RiveRenderTarget.h"
#include "RiveTypeConversions.h"
#include "TimerManager.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Logs/RiveLog.h"
#include "Blueprint/UserWidget.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"

#include "Stats/RiveStats.h"

#include "Rendering/DrawElements.h"
#include "rive/command_server.hpp"
#include "rive/generated/layout/layout_component_style_base.hpp"
#include "rive/intrinsically_sizeable.hpp"
#include "rive/layout_component.hpp"
#include "rive/nested_artboard.hpp"
#include "rive/node.hpp"
#include "Rive/RiveArtboard.h"
#include "Rive/RiveDescriptor.h"
#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/shapes/image.hpp"
#include "rive/shapes/parametric_path.hpp"
#include "rive/shapes/shape.hpp"
#include "rive/text/text.hpp"
#include "Streaming/TextureMipDataProvider.h"

#include <IRenderCaptureProvider.h>
#include <atomic>

DECLARE_GPU_STAT_NAMED(FRiveRendererDrawElementDraw,
                       TEXT("FRiveRendererDrawElement::Draw_RenderThread"));

FORCEINLINE rive::AABB AABBForSlateRect(const FSlateRect& Rect)
{
    return rive::AABB(Rect.Left, Rect.Top, Rect.Right, Rect.Bottom);
}

const TCHAR* LayoutScaleTypeToString(rive::LayoutScaleType ScaleType)
{
    switch (ScaleType)
    {
        case rive::LayoutScaleType::fixed:
            return TEXT("fixed");
        case rive::LayoutScaleType::fill:
            return TEXT("fill");
        case rive::LayoutScaleType::hug:
            return TEXT("hug");
    }
    return TEXT("unknown");
}

const TCHAR* TextSizingToString(rive::TextSizing Sizing)
{
    switch (Sizing)
    {
        case rive::TextSizing::autoWidth:
            return TEXT("autoWidth");
        case rive::TextSizing::autoHeight:
            return TEXT("autoHeight");
        case rive::TextSizing::fixed:
            return TEXT("fixed");
    }
    return TEXT("unknown");
}

void LogRiveLayoutDiagnostics(rive::ArtboardInstance* ArtboardInstance)
{
    const FString ArtboardName =
        UTF8_TO_TCHAR(ArtboardInstance->name().c_str());
    const bool bIsCarousel = ArtboardName.Contains(TEXT("Carousel"));

    static std::atomic<int32> RemainingGeneralDumps{8};
    static std::atomic<int32> RemainingCarouselDumps{24};
    if (bIsCarousel)
    {
        if (RemainingCarouselDumps.fetch_sub(1) <= 0)
        {
            return;
        }
    }
    else if (RemainingGeneralDumps.fetch_sub(1) <= 0)
    {
        return;
    }

    const rive::AABB Bounds = ArtboardInstance->bounds();
    const rive::AABB LayoutBounds = ArtboardInstance->layoutBounds();
    UE_LOG(LogRive,
           Warning,
           TEXT("RiveLayoutDiag artboard=%hs width=%.2f height=%.2f "
                "bounds=(%.2f %.2f %.2f %.2f) layoutBounds=(%.2f %.2f %.2f "
                "%.2f)"),
           ArtboardInstance->name().c_str(),
           ArtboardInstance->width(),
           ArtboardInstance->height(),
           Bounds.left(),
           Bounds.top(),
           Bounds.right(),
           Bounds.bottom(),
           LayoutBounds.left(),
           LayoutBounds.top(),
           LayoutBounds.right(),
           LayoutBounds.bottom());

    const size_t LayoutCount = ArtboardInstance->count<rive::LayoutComponent>();
    for (size_t Index = 0; Index < LayoutCount; ++Index)
    {
        rive::LayoutComponent* Layout =
            ArtboardInstance->objectAt<rive::LayoutComponent>(Index);
        if (!Layout)
        {
            continue;
        }

        UE_LOG(LogRive,
               Warning,
               TEXT("RiveLayoutDiag layout[%llu] name=%hs parent=%hs "
                    "x=%.2f y=%.2f w=%.2f h=%.2f"),
               static_cast<unsigned long long>(Index),
               Layout->name().c_str(),
               Layout->parent() ? Layout->parent()->name().c_str() : "",
               Layout->layoutX(),
               Layout->layoutY(),
               Layout->layoutWidth(),
               Layout->layoutHeight());

        if (rive::LayoutComponentStyle* Style = Layout->style())
        {
            const rive::LayoutComponentStyleBase* StyleBase =
                reinterpret_cast<const rive::LayoutComponentStyleBase*>(Style);
            UE_LOG(LogRive,
                   Warning,
                   TEXT("RiveLayoutDiag layout[%llu] style width=%.2f "
                        "height=%.2f widthUnits=%d heightUnits=%d "
                        "widthScale=%s heightScale=%s intrinsic=%d flex=%.2f "
                        "grow=%.2f shrink=%.2f basis=%.2f basisUnits=%u "
                        "aspect=%.2f"),
                   static_cast<unsigned long long>(Index),
                   Layout->width(),
                   Layout->height(),
                   static_cast<int>(StyleBase->widthUnitsValue()),
                   static_cast<int>(StyleBase->heightUnitsValue()),
                   LayoutScaleTypeToString(static_cast<rive::LayoutScaleType>(
                       StyleBase->layoutWidthScaleType())),
                   LayoutScaleTypeToString(static_cast<rive::LayoutScaleType>(
                       StyleBase->layoutHeightScaleType())),
                   StyleBase->intrinsicallySizedValue(),
                   StyleBase->flex(),
                   StyleBase->flexGrow(),
                   StyleBase->flexShrink(),
                   StyleBase->flexBasis(),
                   StyleBase->flexBasisUnitsValue(),
                   StyleBase->aspectRatio());
        }

        const rive::Vec2D LayoutMeasureUndefined = Layout->measureLayout(
            NAN,
            rive::LayoutMeasureMode::undefined,
            NAN,
            rive::LayoutMeasureMode::undefined);
        const rive::Vec2D LayoutMeasureAtMost = Layout->measureLayout(
            Layout->layoutWidth(),
            rive::LayoutMeasureMode::atMost,
            Layout->layoutHeight(),
            rive::LayoutMeasureMode::atMost);
        UE_LOG(LogRive,
               Warning,
               TEXT("RiveLayoutDiag layout[%llu] measure undefined=(%.2f "
                    "%.2f) atMost=(%.2f %.2f)"),
               static_cast<unsigned long long>(Index),
               LayoutMeasureUndefined.x,
               LayoutMeasureUndefined.y,
               LayoutMeasureAtMost.x,
               LayoutMeasureAtMost.y);

        int32 ChildIndex = 0;
        for (rive::Component* Child : Layout->children())
        {
            rive::IntrinsicallySizeable* Sizeable =
                rive::IntrinsicallySizeable::from(Child);
            const rive::Vec2D ChildMeasureUndefined =
                Sizeable ? Sizeable->measureLayout(
                               NAN,
                               rive::LayoutMeasureMode::undefined,
                               NAN,
                               rive::LayoutMeasureMode::undefined)
                         : rive::Vec2D();
            const rive::Vec2D ChildMeasureAtMost =
                Sizeable ? Sizeable->measureLayout(
                               Layout->layoutWidth(),
                               rive::LayoutMeasureMode::atMost,
                               Layout->layoutHeight(),
                               rive::LayoutMeasureMode::atMost)
                         : rive::Vec2D();
            const bool bIsLayout = Child->is<rive::LayoutComponent>();
            const bool bIsShape = Child->is<rive::Shape>();
            const bool bIsParametricPath = Child->is<rive::ParametricPath>();
            const bool bIsImage = Child->is<rive::Image>();
            const bool bIsNestedArtboard = Child->is<rive::NestedArtboard>();
            const bool bIsNode = Child->is<rive::Node>();
            UE_LOG(LogRive,
                   Warning,
                   TEXT("RiveLayoutDiag layout[%llu] child[%d] name=%hs "
                        "type=%u layout=%d shape=%d path=%d image=%d nested=%d node=%d "
                        "sizeable=%d measureUndefined=(%.2f %.2f) "
                        "measureAtMost=(%.2f %.2f)"),
                   static_cast<unsigned long long>(Index),
                   ChildIndex++,
                   Child->name().c_str(),
                   Child->coreType(),
                   bIsLayout,
                   bIsShape,
                   bIsParametricPath,
                   bIsImage,
                   bIsNestedArtboard,
                   bIsNode,
                   Sizeable != nullptr,
                   ChildMeasureUndefined.x,
                   ChildMeasureUndefined.y,
                   ChildMeasureAtMost.x,
                   ChildMeasureAtMost.y);

            if (bIsNestedArtboard)
            {
                rive::NestedArtboard* Nested =
                    Child->as<rive::NestedArtboard>();
                rive::ArtboardInstance* NestedInstance =
                    Nested ? Nested->artboardInstance() : nullptr;
                if (NestedInstance)
                {
                    const rive::AABB NestedBounds = NestedInstance->bounds();
                    UE_LOG(LogRive,
                           Warning,
                           TEXT("RiveLayoutDiag layout[%llu] child[%d] nestedArtboard=%hs "
                                "width=%.2f height=%.2f layout=(%.2f %.2f) "
                                "bounds=(%.2f %.2f %.2f %.2f) layoutCount=%llu"),
                           static_cast<unsigned long long>(Index),
                           ChildIndex - 1,
                           NestedInstance->name().c_str(),
                           NestedInstance->width(),
                           NestedInstance->height(),
                           NestedInstance->layoutWidth(),
                           NestedInstance->layoutHeight(),
                           NestedBounds.left(),
                           NestedBounds.top(),
                           NestedBounds.right(),
                           NestedBounds.bottom(),
                           static_cast<unsigned long long>(
                               NestedInstance->count<rive::LayoutComponent>()));
                }
            }
        }
    }

    const size_t TextCount = ArtboardInstance->count<rive::Text>();
    for (size_t Index = 0; Index < TextCount; ++Index)
    {
        rive::Text* Text = ArtboardInstance->objectAt<rive::Text>(Index);
        if (!Text)
        {
            continue;
        }
        const rive::AABB TextBounds = Text->localBounds();
        UE_LOG(LogRive,
               Warning,
               TEXT("RiveLayoutDiag text[%llu] name=%hs parent=%hs "
                    "sizing=%s effective=%s effectiveW=%.2f effectiveH=%.2f "
                    "bounds=(%.2f %.2f %.2f %.2f)"),
               static_cast<unsigned long long>(Index),
               Text->name().c_str(),
               Text->parent() ? Text->parent()->name().c_str() : "",
               TextSizingToString(Text->sizing()),
               TextSizingToString(Text->effectiveSizing()),
               Text->effectiveWidth(),
               Text->effectiveHeight(),
               TextBounds.left(),
               TextBounds.top(),
               TextBounds.right(),
               TextBounds.bottom());
    }
}

void LogRiveArtboardLayoutDiagnostic(rive::ArtboardInstance* ArtboardInstance,
                                     const FSlateRect& RenderBounds,
                                     const rive::Fit Fit,
                                     const float TotalScale)
{
    static std::atomic<int32> RemainingDumps{60};
    if (RemainingDumps.fetch_sub(1, std::memory_order_relaxed) <= 0)
    {
        return;
    }

    const rive::AABB Bounds = ArtboardInstance->bounds();
    const rive::AABB LayoutBounds = ArtboardInstance->layoutBounds();
    UE_LOG(LogRive,
           Warning,
           TEXT("RiveArtboardHugDiag name=%hs fit=%d scale=%.3f "
                "artboard=(%.2f %.2f) layout=(%.2f %.2f) "
                "bounds=(%.2f %.2f %.2f %.2f) layoutBounds=(%.2f %.2f %.2f %.2f) "
                "renderBounds=(%.2f %.2f %.2f %.2f)"),
           ArtboardInstance->name().c_str(),
           static_cast<int32>(Fit),
           TotalScale,
           ArtboardInstance->width(),
           ArtboardInstance->height(),
           ArtboardInstance->layoutWidth(),
           ArtboardInstance->layoutHeight(),
           Bounds.left(),
           Bounds.top(),
           Bounds.right(),
           Bounds.bottom(),
           LayoutBounds.left(),
           LayoutBounds.top(),
           LayoutBounds.right(),
           LayoutBounds.bottom(),
           RenderBounds.Left,
           RenderBounds.Top,
           RenderBounds.Right,
           RenderBounds.Bottom);
}

class FRiveRendererDrawElement : public ICustomSlateElement
{
public:
    FRiveRendererDrawElement()
    {
        RiveRenderer = IRiveRendererModule::Get().GetRenderer();
        if (!ensure(RiveRenderer))
        {
            UE_LOG(LogRive,
                   Error,
                   TEXT("FRiveRendererDrawElement() "
                        "RiveRenderer is Null"));
            return;
        }

        CommandServer = RiveRenderer->GetCommandServerUnsafe();
        if (!ensure(CommandServer))
        {
            UE_LOG(LogRive,
                   Error,
                   TEXT("FRiveRendererDrawElement() "
                        "CommandServer is Null"));
            return;
        }
    }
    FRiveRendererDrawElement(TWeakObjectPtr<URiveArtboard> RiveArtboard) :
        RiveArtboard(MoveTemp(RiveArtboard))
    {
        RiveRenderer = IRiveRendererModule::Get().GetRenderer();
        if (!ensure(RiveRenderer))
        {
            UE_LOG(LogRive,
                   Error,
                   TEXT("FRiveRendererDrawElement() "
                        "RiveRenderer is Null"));
            return;
        }

        CommandServer = RiveRenderer->GetCommandServerUnsafe();
        if (!ensure(CommandServer))
        {
            UE_LOG(LogRive,
                   Error,
                   TEXT("FRiveRendererDrawElement() "
                        "CommandServer is Null"));
            return;
        }
    }
    /*
     * We have to bypass the command queue here because it's already on the
     * render thread. This will be thread safe but we should find a way to not
     * have to bypass it
     */
    virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder,
                                   const FDrawPassInputs& Inputs) override
    {
        check(RiveRenderer);
        check(CommandServer);

        if (Inputs.OutputTexture == nullptr)
            return;

        SCOPED_NAMED_EVENT_TEXT(
            TEXT("FRiveRendererDrawElement::Draw_RenderThread"),
            FColor::White);

        DECLARE_SCOPE_CYCLE_COUNTER(
            TEXT("FRiveRendererDrawElement::Draw_RenderThread"),
            STAT_RIVE_RENDER_ELEMENT_DRAW,
            STATGROUP_Rive);

        SCOPED_GPU_STAT(GraphBuilder.RHICmdList, FRiveRendererDrawElementDraw);

        auto RiveArtboardLocal = RiveArtboard.Pin();
        auto RenderBoundsLocal = RenderBounds;

        const float TotalScale = Scale * DPIScale;

        if (!RiveArtboardLocal.IsValid())
        {
            UE_LOG(LogRive,
                   Error,
                   TEXT("FRiveRendererDrawElement::Draw_RenderThread Artboard "
                        "not set"));
            return;
        }
        // TODO: Find a way to create just one render target for the screen
        // texture rather than one per widget (these all end up with the same
        // target texture)
        if (RenderTarget.Get() == nullptr)
        {
            RenderTarget = RiveRenderer->CreateRenderTarget(
                GraphBuilder,
                "FRiveRendererDrawElement::Draw_RenderThread",
                Inputs.OutputTexture);
        }

        auto renderTarget = RenderTarget->GetRenderTarget();
        if (renderTarget->width() != Inputs.OutputTexture->Desc.Extent.X ||
            renderTarget->height() != Inputs.OutputTexture->Desc.Extent.Y)
        {
            RenderTarget = RiveRenderer->CreateRenderTarget(
                GraphBuilder,
                "FRiveRendererDrawElement::Draw_RenderThread",
                Inputs.OutputTexture);
            renderTarget = RenderTarget->GetRenderTarget();
        }

        RenderTarget->UpdateTargetTexture(Inputs.OutputTexture);

        if (!ensure(RenderTarget))
        {
            UE_LOG(LogRive,
                   Error,
                   TEXT("FRiveRendererDrawElement::Draw_RenderThread RDG "
                        "RenderTarget not supported on this platform"));
            return;
        }

        if (!ensure(renderTarget))
        {
            UE_LOG(
                LogRive,
                Error,
                TEXT("FRiveRendererDrawElement::Draw_RenderThread RDG "
                     "rive::gpu::RenderTarget not supported on this platform"));
            return;
        }

        auto ArtboardHandle = RiveArtboardLocal->GetNativeArtboardHandle();
        if (ArtboardHandle == RIVE_NULL_HANDLE)
        {
            UE_LOG(LogRive,
                   Warning,
                   TEXT("FRiveRendererDrawElement::Draw_RenderThread "
                        "ArtboardHandle is invalid"));
            return;
        }

        auto ArtboardInstance =
            CommandServer->getArtboardInstance(ArtboardHandle);
        if (ArtboardInstance == nullptr)
        {
            UE_LOG(LogRive,
                   Warning,
                   TEXT("FRiveRendererDrawElement::Draw_RenderThread "
                        "ArtboardInstance is invalid"));
            return;
        }

        if (LastLayoutInvalidationVersion !=
            RiveArtboardLocal->GetLayoutInvalidationVersion())
        {
            LastLayoutInvalidationVersion =
                RiveArtboardLocal->GetLayoutInvalidationVersion();
            bLayoutDirty = true;
        }

        auto Context = RiveRenderer->GetRenderContext();
        Context->beginFrame({
            .renderTargetWidth =
                static_cast<uint32_t>(Inputs.OutputTexture->Desc.GetSize().X),
            .renderTargetHeight =
                static_cast<uint32_t>(Inputs.OutputTexture->Desc.GetSize().Y),
            .loadAction = rive::gpu::LoadAction::preserveRenderTarget,
            .wireframe = Inputs.bWireFrame,
        });

        rive::RawPath ClipPath;
        ClipPath.addRect(AABBForSlateRect(ClipRect));
        auto ClipRenderPath =
            Context->makeRenderPath(ClipPath, rive::FillRule::nonZero);

        auto Renderer = MakeUnique<rive::RiveRenderer>(Context);
        Renderer->save();
        Renderer->clipPath(ClipRenderPath.get());

        const bool bIsLayoutFit = Fit == rive::Fit::layout;
        const bool bHasValidLayoutScale = TotalScale > 0.0f;
        const FVector2f RenderSize = RenderBoundsLocal.GetSize();

        bool bRefreshedLayout = false;

        if (bIsLayoutFit && bHasValidLayoutScale && RenderSize.X > 0.0f &&
            RenderSize.Y > 0.0f && (bLayoutDirty || bRenderBoundsDirty))
        {
            ArtboardInstance->width(RenderSize.X / TotalScale);
            ArtboardInstance->height(RenderSize.Y / TotalScale);

            // Layout sizing can be overwritten by animation/state-machine
            // updates, so apply it immediately before alignment and draw.
            if (auto NativeStateMachineHandle =
                    RiveArtboardLocal->GetStateMachineHandle();
                NativeStateMachineHandle != RIVE_NULL_HANDLE)
            {
                if (auto StateMachine = CommandServer->getStateMachineInstance(
                        NativeStateMachineHandle))
                {
                    StateMachine->advanceAndApply(0);
                }
            }
            else
            {
                ArtboardInstance->advance(0);
            }

            bRefreshedLayout = true;
        }
        else if (bLayoutDirty)
        {
            ArtboardInstance->resetSize();

            if (auto NativeStateMachineHandle =
                    RiveArtboardLocal->GetStateMachineHandle();
                NativeStateMachineHandle != RIVE_NULL_HANDLE)
            {
                if (auto StateMachine = CommandServer->getStateMachineInstance(
                        NativeStateMachineHandle))
                {
                    StateMachine->advanceAndApply(0);
                }
            }
            else
            {
                ArtboardInstance->advance(0);
            }

            bRefreshedLayout = true;
        }

        bLayoutDirty = false;
        bRenderBoundsDirty = false;

        if (bRefreshedLayout)
        {
            LogRiveLayoutDiagnostics(ArtboardInstance);
        }

        Renderer->align(Fit,
                        Alignment,
                        AABBForSlateRect(RenderBoundsLocal),
                        ArtboardInstance->bounds(),
                        TotalScale);
        if (bRefreshedLayout)
        {
            LogRiveArtboardLayoutDiagnostic(ArtboardInstance,
                                            RenderBoundsLocal,
                                            Fit,
                                            TotalScale);
        }
        ArtboardInstance->draw(Renderer.Get());
        Renderer->restore();

        // This is left as a comment because it could be useful later,
        // This had the abililty to capture a frame of only rive but its very
        // slow to leave in

        // if (IRenderCaptureProvider::IsAvailable())
        //{
        //     GraphBuilder.AddPass(RDG_EVENT_NAME("End Frame Capture"),
        //                          ERDGPassFlags::NeverCull,
        //                          [](FRHICommandListImmediate& RHICmdList) {
        //                              IRenderCaptureProvider::Get().BeginCapture(
        //                                  &RHICmdList,
        //                                  0,
        //                                  FString());
        //                          });
        //
        //     Context->flush({.renderTarget = renderTarget.get(),
        //                     .externalCommandBuffer = &GraphBuilder});
        //
        //     GraphBuilder.AddPass(
        //         RDG_EVENT_NAME("Begin Frame Capture"),
        //                         ERDGPassFlags::NeverCull,
        //                         [](FRHICommandListImmediate& RHICmdList) {
        //                             IRenderCaptureProvider::Get().EndCapture(
        //                                 &RHICmdList);
        //                         });
        // }
        //  else
        {
            Context->flush({.renderTarget = renderTarget.get(),
                            .externalCommandBuffer = &GraphBuilder});
        }
    }

    void SetArtboard(TWeakObjectPtr<URiveArtboard> InRiveArtboard)
    {
        RiveArtboard = InRiveArtboard;
        LastLayoutInvalidationVersion = 0;
        bLayoutDirty = true;
    }

    void SetFromDescriptor(const FRiveDescriptor& InRiveDescriptor)
    {
        Alignment = RiveAlignementToAlignment(InRiveDescriptor.Alignment);
        Fit = RiveFitTypeToFit(InRiveDescriptor.FitType);
        Scale = InRiveDescriptor.ScaleFactor;
        bLayoutDirty = true;
        bRenderBoundsDirty = true;
    }

    void SetDPIScale(float InDPIScale)
    {
        if (!FMath::IsNearlyEqual(DPIScale, InDPIScale))
        {
            DPIScale = InDPIScale;
            bRenderBoundsDirty = true;
        }
    }

    void SetRenderingBounds(const FSlateRect& InRenderBounds)
    {
        bRenderBoundsDirty |= RenderBounds != InRenderBounds;
        RenderBounds = InRenderBounds;
    }

    void SetClipRect(const FSlateRect& InClipRect) { ClipRect = InClipRect; }

private:
    FRiveRenderer* RiveRenderer = nullptr;
    rive::CommandServer* CommandServer = nullptr;
    TSharedPtr<FRiveRenderTarget> RenderTarget;
    TWeakObjectPtr<URiveArtboard> RiveArtboard;
    // Locally copy of descriptor so we can pass it to the renderer
    rive::Alignment Alignment;
    rive::Fit Fit;
    float Scale = 1;
    float DPIScale = 1;
    FSlateRect RenderBounds;
    FSlateRect ClipRect;
    uint32 LastLayoutInvalidationVersion = 0;
    bool bLayoutDirty = true;
    bool bRenderBoundsDirty = true;
};

void SRiveLeafWidget::SetRiveDescriptor(const FRiveDescriptor& InDescriptor)
{
    RiveRendererDrawElement->SetFromDescriptor(InDescriptor);
    bScaleByDPI = InDescriptor.bScaleLayoutByDPI;
}

void SRiveLeafWidget::SetArtboard(TWeakObjectPtr<URiveArtboard> InArtboard)
{
    Artboard = InArtboard;
    RiveRendererDrawElement->SetArtboard(InArtboard);
}

void SRiveLeafWidget::Construct(const FArguments& InArgs)
{
    FRiveDescriptor Descriptor;
    if (InArgs._OwningWidget.IsSet())
        OwningWidget = InArgs._OwningWidget.Get();
    if (InArgs._Artboard.IsSet())
        Artboard = InArgs._Artboard.Get();
    if (InArgs._Descriptor.IsSet())
        Descriptor = InArgs._Descriptor.Get();

    RiveRendererDrawElement = MakeShared<FRiveRendererDrawElement>(Artboard);
    RiveRendererDrawElement->SetFromDescriptor(Descriptor);
}

int32 SRiveLeafWidget::OnPaint(const FPaintArgs& Args,
                               const FGeometry& AllottedGeometry,
                               const FSlateRect& MyCullingRect,
                               FSlateWindowElementList& OutDrawElements,
                               int32 LayerId,
                               const FWidgetStyle& InWidgetStyle,
                               bool bParentEnabled) const
{
    if (!bParentEnabled)
        return LayerId;

    // Don't try to draw if we don't have an artboard.
    if (!Artboard.IsValid())
        return LayerId;

    if (bScaleByDPI && IsValid(OwningWidget))
    {
        const float Scale =
            UWidgetLayoutLibrary::GetViewportScale(OwningWidget);
        RiveRendererDrawElement->SetDPIScale(Scale);
    }
    else
    {
        RiveRendererDrawElement->SetDPIScale(1.0f);
    }

    RiveRendererDrawElement->SetRenderingBounds(
        AllottedGeometry.GetRenderBoundingRect());
    RiveRendererDrawElement->SetClipRect(MyCullingRect);
    FSlateDrawElement::MakeCustom(OutDrawElements,
                                  LayerId,
                                  RiveRendererDrawElement);
    return LayerId;
}

FVector2D SRiveLeafWidget::ComputeDesiredSize(float) const
{
    if (auto StrArtboard = Artboard.Pin())
    {
        if (StrArtboard->HasData())
        {
            return StrArtboard->ArtboardDefaultSize;
        }
    }

    return FVector2D(256, 256);
}
