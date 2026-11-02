// SPDX-License-Identifier: GPL-3.0-only
// Verbatim changed src/interop.cpp consumer transitions. CPU dependencies are
// labeled in consumer_completion_cpu.h; public contracts are actual interop.h.
namespace nr::interop {
std::optional<Consumer> Bridge::acquire_raw(const Ticket& t){
 if(!impl_||!impl_->on_graphics())return std::nullopt;auto* s=impl_->find(t);
 if(!s||s->failed||s->consumerOpen||impl_->producer11->GetCompletedValue()==UINT64_MAX||
    impl_->consumer11->GetCompletedValue()==UINT64_MAX)return std::nullopt;
 const auto value=impl_->leaseCursor.reserve();if(!value)return std::nullopt;
 s->consumerLease=value;s->consumerRaw=true;s->consumerOpen=true;
 return Consumer{t,s->raw.Get(),value,true,this,impl_->consumer11,impl_->context->graphics_thread()};
}
std::optional<Consumer> Bridge::acquire_output(const Ticket& t){
 if(!impl_||!impl_->on_graphics())return std::nullopt;auto* s=impl_->find(t);
 if(!s||s->consumerOpen||!publishable(impl_->snapshot(t.slot),t.key))return std::nullopt;
 const auto value=impl_->leaseCursor.reserve();if(!value)return std::nullopt;
 s->consumerLease=value;s->consumerRaw=false;s->consumerOpen=true;
 return Consumer{t,s->output11.Get(),value,false,this,impl_->consumer11,impl_->context->graphics_thread()};
}
bool Bridge::owns_consumer(const Consumer& c)const{
 if(!impl_||!impl_->on_graphics()||c.bridge!=this||c.fence.Get()!=impl_->consumer11.Get()||
    c.graphicsOwner!=impl_->context->graphics_thread())return false;auto* s=impl_->find(c.ticket);
 return s&&s->consumerOpen&&!s->failed&&c.leaseValue&&s->consumerLease==c.leaseValue&&
    s->consumerRaw==c.raw&&c.texture==(c.raw?s->raw.Get():s->output11.Get());
}
bool Bridge::copy_to(const Consumer& c,ID3D11Texture2D* target){
 if(!owns_consumer(c)||!impl_->shape(target,source_format(impl_->descriptor.format)))return false;
 auto* s=impl_->find(c.ticket);
 try{impl_->convert(c.texture,target);}catch(const std::exception&){s->failed=true;return false;}return true;
}
std::shared_ptr<ConsumerEvent> ConsumerEvent::create(){
 const auto handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 return handle?std::shared_ptr<ConsumerEvent>(new ConsumerEvent(handle)):nullptr;
}
ConsumerEvent::~ConsumerEvent(){CloseHandle(handle_);}
std::shared_ptr<const ConsumerEnd> Bridge::end_consumer(const Consumer& c,std::shared_ptr<ConsumerEvent> event){
 if(!owns_consumer(c)||impl_->consumer11->GetCompletedValue()==UINT64_MAX)return {};
 if(event&&(event->bound_||WaitForSingleObject(event->handle_,0)!=WAIT_TIMEOUT))return {};
 auto* s=impl_->find(c.ticket);
 // This original private fence has one exclusive submission-order cursor. Reserve
 // only here, after the caller's last copy, with no callback/yield before Signal.
 const auto value=impl_->consumerCursor.reserve();if(!value)return {};
 s->consumer=value;s->endEvent=event;
 const auto receipt=std::make_shared<const ConsumerEnd>(ConsumerEnd{c,value,event});
 if(event){
  event->bound_=true;
  if(FAILED(impl_->consumer11->SetEventOnCompletion(value,event->handle_))){s->failed=true;return {};}
 }
 if(FAILED(impl_->graphics->Signal(impl_->consumer11.Get(),value))){s->failed=true;return {};}
 s->endReceipt=receipt;impl_->graphics->Flush();s->consumerOpen=false;return receipt;
}
bool Bridge::cancel(const Ticket& t){if(!impl_||!impl_->on_graphics())return false;
 auto* s=impl_->find(t);if(!s)return false;s->cancelled=true;return true;}
Ownership Bridge::ownership(size_t index)const{return impl_?impl_->snapshot(index):Ownership{std::nullopt,WorkPhase::Empty,0,0,0,0,0,0,false,false,false};}
bool Bridge::quiescent()const{if(!impl_)return closed_;for(size_t i=0;i<3;++i)if(!reusable(impl_->snapshot(i)))return false;return true;}
bool Bridge::close(){if(!impl_)return closed_;if(!impl_->on_graphics()||!quiescent())return false;impl_.reset();closed_=true;return true;}
ID3D11Fence* Bridge::consumer_fence()const{return impl_?impl_->consumer11.Get():nullptr;}
}
