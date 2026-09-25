package com.gagaai.app.widget

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.widget.ScrollView

/**
 * 日志区内层滚动条：解决整页 ScrollView 套日志 ScrollView 的同向嵌套滚动冲突。
 * 默认行为是外层在滑动越过 touchSlop 时抢先拦截手势，日志区永远拖不动、只能滚整页。
 * 这里在按下（手势起点）就向父级申请独占本次手势（requestDisallowInterceptTouchEvent），
 * 父级在整个手势期间不再拦截，日志区自己滚动；若内容已顶/到底（该方向无剩余可滚），
 * 则不申请，手势自然交还外层滚整页。
 */
class LogScrollView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = android.R.attr.scrollViewStyle,
) : ScrollView(context, attrs, defStyleAttr) {

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_DOWN &&
            (canScrollVertically(-1) || canScrollVertically(1))
        ) {
            parent?.requestDisallowInterceptTouchEvent(true)
        }
        return super.onInterceptTouchEvent(ev)
    }
}
