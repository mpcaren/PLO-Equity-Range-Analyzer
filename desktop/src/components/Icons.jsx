/* Small stroke icon set (24px grid, currentColor). */

const base = {
  width: 18, height: 18, viewBox: "0 0 24 24", fill: "none",
  stroke: "currentColor", strokeWidth: 1.8, strokeLinecap: "round", strokeLinejoin: "round",
  "aria-hidden": true,
};

const I = (paths) => function Icon({ size = 18, ...rest }) {
  return <svg {...base} width={size} height={size} {...rest}>{paths}</svg>;
};

export const IconEquity = I(<><path d="M12 3v9l7.8 4.5" /><circle cx="12" cy="12" r="9" /></>);
export const IconTarget = I(<><circle cx="12" cy="12" r="9" /><circle cx="12" cy="12" r="5" /><circle cx="12" cy="12" r="1.2" fill="currentColor" /></>);
export const IconStack = I(<><ellipse cx="12" cy="6" rx="7" ry="2.6" /><path d="M5 6v4c0 1.4 3.1 2.6 7 2.6s7-1.2 7-2.6V6" /><path d="M5 10v4c0 1.4 3.1 2.6 7 2.6s7-1.2 7-2.6v-4" /><path d="M5 14v4c0 1.4 3.1 2.6 7 2.6s7-1.2 7-2.6v-4" /></>);
export const IconSettings = I(<><circle cx="12" cy="12" r="3" /><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1A1.7 1.7 0 0 0 9 19.4a1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1A1.7 1.7 0 0 0 4.6 9a1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z" /></>);
export const IconBolt = I(<path d="M13 2 4 14h7l-1 8 9-12h-7l1-8z" />);
export const IconStop = I(<rect x="6" y="6" width="12" height="12" rx="2.5" />);
export const IconDice = I(<><rect x="3.5" y="3.5" width="17" height="17" rx="4" /><circle cx="8.5" cy="8.5" r="1.1" fill="currentColor" /><circle cx="15.5" cy="15.5" r="1.1" fill="currentColor" /><circle cx="15.5" cy="8.5" r="1.1" fill="currentColor" /><circle cx="8.5" cy="15.5" r="1.1" fill="currentColor" /><circle cx="12" cy="12" r="1.1" fill="currentColor" /></>);
export const IconCopy = I(<><rect x="9" y="9" width="11" height="11" rx="2.5" /><path d="M5 15V6a2 2 0 0 1 2-2h8" /></>);
export const IconTrash = I(<><path d="M4 7h16" /><path d="M10 11v6M14 11v6" /><path d="M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12" /><path d="M9 7V4h6v3" /></>);
export const IconX = I(<path d="M6 6l12 12M18 6 6 18" />);
export const IconPlus = I(<path d="M12 5v14M5 12h14" />);
export const IconMinus = I(<path d="M5 12h14" />);
export const IconBars = I(<><path d="M4 20h16" /><rect x="5" y="11" width="3" height="6" rx="1" /><rect x="10.5" y="6" width="3" height="11" rx="1" /><rect x="16" y="9" width="3" height="8" rx="1" /></>);
export const IconSparkle = I(<path d="M12 3l1.8 5.2L19 10l-5.2 1.8L12 17l-1.8-5.2L5 10l5.2-1.8L12 3zM19 16l.8 2.2L22 19l-2.2.8L19 22l-.8-2.2L16 19l2.2-.8L19 16z" />);
export const IconFilter = I(<path d="M4 5h16l-6 7.5V19l-4 2v-8.5L4 5z" />);
export const IconChevron = I(<path d="m6 9 6 6 6-6" />);
export const IconArrowRight = I(<path d="M5 12h14M13 6l6 6-6 6" />);
export const IconKeyboard = I(<><rect x="2.5" y="6" width="19" height="12" rx="2.5" /><path d="M6 10h.01M10 10h.01M14 10h.01M18 10h.01M7 14h10" /></>);
export const IconFolder = I(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7z" />);
export const IconCheck = I(<path d="m5 12.5 4.5 4.5L19 7.5" />);
export const IconAlert = I(<><path d="M12 9v4M12 17h.01" /><path d="M10.3 3.9 2.6 17.5A2 2 0 0 0 4.3 20.5h15.4a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z" /></>);
export const IconRefresh = I(<><path d="M20 11a8 8 0 0 0-14.9-3M4 4v4h4" /><path d="M4 13a8 8 0 0 0 14.9 3M20 20v-4h-4" /></>);
export const IconCards = I(<><rect x="3" y="5" width="11" height="15" rx="2" transform="rotate(-8 8.5 12.5)" /><rect x="10" y="4" width="11" height="15" rx="2" transform="rotate(8 15.5 11.5)" /></>);
